import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


BOARD_DIR = Path(__file__).resolve().parents[1]
VALIDATOR = BOARD_DIR / "tools" / "validate_assets.py"
BUILDER = BOARD_DIR / "tools" / "build_assets.mjs"
EXPECTED_EMOTIONS = {
    "neutral",
    "happy",
    "laughing",
    "funny",
    "sad",
    "angry",
    "crying",
    "loving",
    "embarrassed",
    "surprised",
    "shocked",
    "thinking",
    "winking",
    "cool",
    "relaxed",
    "delicious",
    "kissy",
    "confident",
    "sleepy",
    "silly",
    "confused",
}


def _expected_source_sha256() -> dict[str, str]:
    manifest = json.loads(
        (BOARD_DIR / "assets-manifest.json").read_text(encoding="utf-8")
    )
    return {
        entry["name"]: entry["source_sha256"]
        for entry in manifest["emoji_entries"]
    }


def _fixed_string(value: str, size: int = 32) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > size:
        raise ValueError(value)
    return encoded.ljust(size, b"\0")


def _parse_srmodel_files(payload: bytes) -> dict[str, dict[str, bytes]]:
    model_count = struct.unpack_from("<I", payload)[0]
    cursor = 4
    entries: list[tuple[str, list[tuple[str, int, int]]]] = []
    for _ in range(model_count):
        model_name = payload[cursor : cursor + 32].split(b"\0", 1)[0].decode()
        cursor += 32
        file_count = struct.unpack_from("<I", payload, cursor)[0]
        cursor += 4
        files: list[tuple[str, int, int]] = []
        for _ in range(file_count):
            file_name = payload[cursor : cursor + 32].split(b"\0", 1)[0].decode()
            start, size = struct.unpack_from("<II", payload, cursor + 32)
            cursor += 40
            files.append((file_name, start, size))
        entries.append((model_name, files))
    return {
        model_name: {
            file_name: payload[start : start + size]
            for file_name, start, size in files
        }
        for model_name, files in entries
    }


def _canonical_wakeword_files() -> dict[str, bytes]:
    assets = _parse_assets_files((BOARD_DIR / "assets.bin").read_bytes())
    return _parse_srmodel_files(assets["srmodels.bin"])["wn9_heyily_tts2"]


def _build_srmodels(
    model_names: list[str],
    *,
    substitute_expected_model: bool = False,
    append_unreferenced_data: bool = False,
) -> bytes:
    models: list[tuple[str, dict[str, bytes]]] = []
    for model_name in model_names:
        if model_name == "wn9_heyily_tts2":
            files = _canonical_wakeword_files()
            if substitute_expected_model:
                files = {**files, "wn9_data": b"substituted WakeNet payload"}
        else:
            files = {"_MODEL_INFO_": f"model:{model_name}".encode()}
        models.append((model_name, files))
    header_size = 4 + sum(
        32 + 4 + len(files) * (32 + 4 + 4) for _model_name, files in models
    )
    headers = bytearray(struct.pack("<I", len(model_names)))
    data = bytearray()
    for model_name, files in models:
        headers.extend(_fixed_string(model_name))
        headers.extend(struct.pack("<I", len(files)))
        for file_name, payload in files.items():
            headers.extend(_fixed_string(file_name))
            headers.extend(struct.pack("<II", header_size + len(data), len(payload)))
            data.extend(payload)
    payload = bytes(headers + data)
    if append_unreferenced_data:
        payload += b"unreferenced model data"
    return payload


def _build_assets(
    files: dict[str, bytes], *, append_unreferenced_data: bool = False
) -> bytes:
    table = bytearray()
    merged = bytearray()
    for file_name, payload in files.items():
        offset = len(merged)
        merged.extend(b"ZZ")
        merged.extend(payload)
        table.extend(_fixed_string(file_name))
        table.extend(struct.pack("<IIHH", len(payload), offset, 0, 0))
    combined = table + merged
    if append_unreferenced_data:
        combined += b"unreferenced asset data"
    checksum = sum(combined) & 0xFFFF
    return struct.pack("<III", len(files), checksum, len(combined)) + combined


def _parse_assets_files(payload: bytes) -> dict[str, bytes]:
    file_count, _checksum, _combined_size = struct.unpack_from("<III", payload)
    table = payload[12 : 12 + file_count * 44]
    data = payload[12 + file_count * 44 :]
    files: dict[str, bytes] = {}
    for index in range(file_count):
        entry = table[index * 44 : (index + 1) * 44]
        name = entry[:32].split(b"\0", 1)[0].decode("utf-8")
        size, offset = struct.unpack_from("<II", entry, 32)
        files[name] = data[offset + 2 : offset + 2 + size]
    return files


def _write_bundle(
    root: Path,
    model_names: list[str],
    *,
    substitute_expected_model: bool = False,
    substitute_expected_font: bool = False,
    append_unreferenced_asset_data: bool = False,
    append_unreferenced_model_data: bool = False,
) -> Path:
    canonical_assets = _parse_assets_files((BOARD_DIR / "assets.bin").read_bytes())
    font_payload = canonical_assets["font_noto_sans_common_16_4.bin"]
    if substitute_expected_font:
        font_payload = b"substituted common font"
    license_payloads = {
        "LICENSE.otto-emoji-gif.txt": (
            BOARD_DIR / "LICENSES" / "otto-emoji-gif-component.LICENSE"
        ).read_bytes(),
        "LICENSE.xiaozhi-fonts.txt": (
            BOARD_DIR / "LICENSES" / "xiaozhi-fonts.Apache-2.0.LICENSE"
        ).read_bytes(),
        "LICENSE.esp-sr.txt": (
            BOARD_DIR / "LICENSES" / "esp-sr.ESPRESSIF-MIT.LICENSE"
        ).read_bytes(),
    }
    emoji_payloads = {
        name: b"GIF89a" + name.encode("ascii") for name in sorted(EXPECTED_EMOTIONS)
    }
    index = {
        "version": 1,
        "srmodels": "srmodels.bin",
        "text_font": "font_noto_sans_common_16_4.bin",
        "text_font_meta": {
            "bundle": "noto-v1",
            "charset": "common",
            "size": 16,
            "bpp": 4,
        },
        "emoji_collection": [
            {"name": name, "file": f"{name}.gif"}
            for name in sorted(EXPECTED_EMOTIONS)
        ],
    }
    files = {
        "index.json": json.dumps(index, separators=(",", ":")).encode(),
        "srmodels.bin": _build_srmodels(
            model_names,
            substitute_expected_model=substitute_expected_model,
            append_unreferenced_data=append_unreferenced_model_data,
        ),
        "font_noto_sans_common_16_4.bin": font_payload,
        **license_payloads,
        **{f"{name}.gif": payload for name, payload in emoji_payloads.items()},
    }
    assets = _build_assets(
        files, append_unreferenced_data=append_unreferenced_asset_data
    )
    assets_path = root / "assets.bin"
    assets_path.write_bytes(assets)
    (root / "LICENSES").mkdir()
    (root / "LICENSES" / "otto-emoji-gif-component.LICENSE").write_text(
        (BOARD_DIR / "LICENSES" / "otto-emoji-gif-component.LICENSE").read_text(
            encoding="utf-8"
        ),
        encoding="utf-8",
    )
    (root / "LICENSES" / "xiaozhi-fonts.Apache-2.0.LICENSE").write_bytes(
        (BOARD_DIR / "LICENSES" / "xiaozhi-fonts.Apache-2.0.LICENSE").read_bytes()
    )
    (root / "LICENSES" / "esp-sr.ESPRESSIF-MIT.LICENSE").write_bytes(
        (BOARD_DIR / "LICENSES" / "esp-sr.ESPRESSIF-MIT.LICENSE").read_bytes()
    )
    manifest = {
        "schema_version": 1,
        "bundle_id": "synthetic",
        "target_board": "irille-s3-eye",
        "partition_limit_bytes": 0x200000,
        "generator": {
            "repository": "https://github.com/78/xiaozhi-assets-generator",
            "commit": "55517b40d724014faff00f941ca700cbf9d14b51",
        },
        "wakeword": {
            "display": "Hey,Ily",
            "model": "wn9_heyily_tts2",
            "component": "espressif/esp-sr",
            "version": "2.4.7",
            "component_hash": (
                "809d0041cdddd98a278f0d5afef7bb60a451290577b98cf718dfffc91bdcbd9b"
            ),
            "repository_commit": "2f8c4b0459db5bbb39abd77adae27962d6d94bcb",
            "license": "ESPRESSIF-MIT",
            "files": [
                {
                    "name": "_MODEL_INFO_",
                    "sha256": "dc4db6b880e0d9511575e93c13e2bc61167076650c4ffdd576c82d59fa67a4e8",
                },
                {
                    "name": "wn9_data",
                    "sha256": "3ea182aa12253d6acd3e8c5c48037150568b0f9fcfb8a5bbfc614fce4b25f4a2",
                },
                {
                    "name": "wn9_index",
                    "sha256": "f13338e279d66ecbac972424a3be4c6184708e6019b9aae613243e83bc6230f9",
                },
            ],
        },
        "emoji_source": {
            "repository": "https://github.com/txp666/otto-emoji-gif-component",
            "commit": "970cf66906d7c30059faa2704e7002f06b8c3619",
            "license": "MIT",
        },
        "text_font": {
            "component": "78/xiaozhi-fonts",
            "version": "2.0.0",
            "component_hash": (
                "3f4f9fa5dcb703208bf22e46a185bb2b4cc9d002b9474b90f7c77e55e0ac541a"
            ),
            "repository_commit": "d45dbc64052d57048f20ab1770074172ce9eb53b",
            "license": "Apache-2.0",
            "file": "font_noto_sans_common_16_4.bin",
            "sha256": hashlib.sha256(font_payload).hexdigest(),
            "bundle": "noto-v1",
            "charset": "common",
            "size": 16,
            "bpp": 4,
        },
        "emoji_entries": [
            {
                "name": name,
                "file": f"{name}.gif",
                "source_sha256": _expected_source_sha256()[name],
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            for name, payload in sorted(emoji_payloads.items())
        ],
        "license_files": [
            {
                "asset_file": asset_file,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            for asset_file, payload in license_payloads.items()
        ],
        "assets": {
            "file": "assets.bin",
            "size_bytes": len(assets),
            "sha256": hashlib.sha256(assets).hexdigest(),
        },
    }
    manifest_path = root / "assets-manifest.json"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    return manifest_path


def _run_validator(manifest_path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), str(manifest_path)],
        check=False,
        capture_output=True,
        text=True,
    )


def _write_candidate(root: Path, **overrides: object) -> Path:
    candidate = {
        "candidate_id": "iris-dual-wakenet9",
        "phrases": ["你好爱莉丝", "Hi Iris"],
        "models": [
            {
                "model_name": "wn9_iris_dual",
                "model_family": "WakeNet9",
                "delivery_shape": "multi-keyword",
                "format": "srmodels-v1",
            }
        ],
        "source": {
            "supplier": "Espressif",
            "license": "non-exclusive-commercial",
        },
        "compatibility": {
            "target": "esp32s3",
            "esp_idf": "6.0.2",
            "esp_sr": "2.4.7",
        },
    }
    candidate.update(overrides)
    path = root / "wakeword-candidate.json"
    path.write_text(json.dumps(candidate), encoding="utf-8")
    return path


def _run_candidate_validator(candidate_path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(VALIDATOR), "--candidate-model", str(candidate_path)],
        check=False,
        capture_output=True,
        text=True,
    )


def _run_repository_check(
    repository_path: Path, expected_commit: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "node",
            str(BUILDER),
            "--check-repository",
            str(repository_path),
            expected_commit,
        ],
        check=False,
        capture_output=True,
        text=True,
    )


class AssetsValidatorTests(unittest.TestCase):
    def test_accepts_valid_synthetic_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(Path(temp), ["wn9_heyily_tts2"])
            result = _run_validator(manifest)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_a_second_wakenet_model(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp), ["wn9_heyily_tts2", "wn9_nihaoxiaozhi_tts"]
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exactly one WakeNet model", result.stderr)

    def test_rejects_substituted_pinned_wakenet_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp),
                ["wn9_heyily_tts2"],
                substitute_expected_model=True,
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("WakeNet payload SHA-256 mismatch", result.stderr)

    def test_rejects_corrupt_assets_checksum(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_heyily_tts2"])
            assets_path = root / "assets.bin"
            corrupted = bytearray(assets_path.read_bytes())
            corrupted[-1] ^= 0xFF
            assets_path.write_bytes(corrupted)
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["assets"]["sha256"] = hashlib.sha256(corrupted).hexdigest()
            manifest.write_text(json.dumps(data), encoding="utf-8")
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("checksum", result.stderr)

    def test_rejects_unreferenced_assets_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp),
                ["wn9_heyily_tts2"],
                append_unreferenced_asset_data=True,
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("assets data ends with unreferenced bytes", result.stderr)

    def test_rejects_unreferenced_srmodels_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp),
                ["wn9_heyily_tts2"],
                append_unreferenced_model_data=True,
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("srmodels data ends with unreferenced bytes", result.stderr)

    def test_rejects_wrong_font_repository_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_heyily_tts2"])
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["text_font"]["repository_commit"] = "0" * 40
            manifest.write_text(json.dumps(data), encoding="utf-8")
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("text font repository commit mismatch", result.stderr)

    def test_rejects_substituted_pinned_font_payload(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp),
                ["wn9_heyily_tts2"],
                substitute_expected_font=True,
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("text font SHA-256 mismatch", result.stderr)

    def test_rejects_assets_filename_that_build_does_not_flash(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_heyily_tts2"])
            data = json.loads(manifest.read_text(encoding="utf-8"))
            (root / "candidate.bin").write_bytes((root / "assets.bin").read_bytes())
            data["assets"]["file"] = "candidate.bin"
            manifest.write_text(json.dumps(data), encoding="utf-8")
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("assets.file must be assets.bin", result.stderr)

    def test_rejects_source_hash_that_does_not_match_pinned_otto_input(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_heyily_tts2"])
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["emoji_entries"][0]["source_sha256"] = "0" * 64
            manifest.write_text(json.dumps(data), encoding="utf-8")
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("emoji source_sha256 mismatch", result.stderr)

    def test_rejects_truncated_otto_license(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_heyily_tts2"])
            (root / "LICENSES" / "otto-emoji-gif-component.LICENSE").write_text(
                "MIT License\nnot the pinned license\n", encoding="utf-8"
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "license SHA-256 mismatch: otto-emoji-gif-component.LICENSE",
                result.stderr,
            )

    def test_rejects_dirty_pinned_source_repository(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            tracked = root / "tracked.txt"
            tracked.write_text("pinned\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", "tracked.txt"], check=True)
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "-c",
                    "user.name=Asset Test",
                    "-c",
                    "user.email=asset-test@example.invalid",
                    "commit",
                    "-q",
                    "-m",
                    "fixture",
                ],
                check=True,
            )
            expected_commit = subprocess.run(
                ["git", "-C", str(root), "rev-parse", "HEAD"],
                check=True,
                capture_output=True,
                text=True,
            ).stdout.strip()
            self.assertEqual(_run_repository_check(root, expected_commit).returncode, 0)
            tracked.write_text("modified\n", encoding="utf-8")
            result = _run_repository_check(root, expected_commit)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("source repository is dirty", result.stderr)


class WakeWordCandidateTests(unittest.TestCase):
    def test_accepts_one_dual_keyword_wakenet9_model(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            candidate = _write_candidate(Path(temp))
            result = _run_candidate_validator(candidate)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_two_separate_models_without_wrapper_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            candidate = _write_candidate(
                Path(temp),
                models=[
                    {
                        "model_name": "wn9_nihao_iris",
                        "model_family": "WakeNet9",
                        "delivery_shape": "separate-model",
                        "format": "srmodels-v1",
                    },
                    {
                        "model_name": "wn9_hi_iris",
                        "model_family": "WakeNet9",
                        "delivery_shape": "separate-model",
                        "format": "srmodels-v1",
                    },
                ],
            )
            result = _run_candidate_validator(candidate)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("wrapper mapping and resource proof", result.stderr)

    def test_rejects_noncommercial_license(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            candidate = _write_candidate(
                Path(temp),
                source={"supplier": "Espressif", "license": "noncommercial"},
            )
            result = _run_candidate_validator(candidate)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("non-exclusive commercial", result.stderr)

    def test_rejects_incompatible_model_format(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            candidate = _write_candidate(
                Path(temp),
                models=[
                    {
                        "model_name": "iris-openwakeword",
                        "model_family": "openWakeWord",
                        "delivery_shape": "multi-keyword",
                        "format": "tflite",
                    }
                ],
            )
            result = _run_candidate_validator(candidate)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("WakeNet9 srmodels-v1", result.stderr)

    def test_rejects_incompatible_runtime_versions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            candidate = _write_candidate(
                Path(temp),
                compatibility={
                    "target": "esp32s3",
                    "esp_idf": "5.5.1",
                    "esp_sr": "2.3.0",
                },
            )
            result = _run_candidate_validator(candidate)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("ESP-IDF 6.0.2 and ESP-SR 2.4.7", result.stderr)


class BoardContractTests(unittest.TestCase):
    def test_real_board_bundle_passes_validator(self) -> None:
        result = _run_validator(BOARD_DIR / "assets-manifest.json")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_real_bundle_carries_readable_local_common_text_font(self) -> None:
        result = _run_validator(BOARD_DIR / "assets-manifest.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertIn("text_font", report)
        self.assertEqual(
            report["text_font"],
            {
                "bundle": "noto-v1",
                "charset": "common",
                "size": 16,
                "bpp": 4,
            },
        )

    def test_real_bundle_embeds_required_third_party_licenses(self) -> None:
        files = _parse_assets_files((BOARD_DIR / "assets.bin").read_bytes())
        for file_name, local_name in (
            ("LICENSE.otto-emoji-gif.txt", "otto-emoji-gif-component.LICENSE"),
            ("LICENSE.xiaozhi-fonts.txt", "xiaozhi-fonts.Apache-2.0.LICENSE"),
            ("LICENSE.esp-sr.txt", "esp-sr.ESPRESSIF-MIT.LICENSE"),
        ):
            self.assertIn(file_name, files)
            self.assertEqual(
                files[file_name], (BOARD_DIR / "LICENSES" / local_name).read_bytes()
            )

    def test_board_selects_only_hey_ily_custom_assets(self) -> None:
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(len(config["builds"]), 1)
        sdkconfig = set(config["builds"][0]["sdkconfig_append"])
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n", sdkconfig)
        self.assertIn("CONFIG_SR_WN_WN9_HIESP=n", sdkconfig)
        self.assertIn("CONFIG_SR_WN_WN9_SOPHIA_TTS=n", sdkconfig)
        self.assertIn("CONFIG_SR_WN_WN9_HEYILY_TTS2=y", sdkconfig)
        self.assertIn("CONFIG_FLASH_CUSTOM_ASSETS=y", sdkconfig)
        self.assertIn(
            'CONFIG_CUSTOM_ASSETS_FILE="boards/irille-s3-eye/assets.bin"',
            sdkconfig,
        )

    def test_real_manifest_provenance_schema_is_complete(self) -> None:
        manifest = json.loads(
            (BOARD_DIR / "assets-manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(
            set(manifest),
            {
                "schema_version",
                "bundle_id",
                "target_board",
                "partition_limit_bytes",
                "generator",
                "wakeword",
                "emoji_source",
                "text_font",
                "emoji_entries",
                "license_files",
                "assets",
            },
        )
        self.assertEqual(set(manifest["generator"]), {"repository", "commit"})
        self.assertEqual(
            set(manifest["wakeword"]),
            {
                "display",
                "model",
                "component",
                "version",
                "component_hash",
                "repository_commit",
                "license",
                "files",
            },
        )
        self.assertEqual(
            {entry["name"] for entry in manifest["wakeword"]["files"]},
            {"_MODEL_INFO_", "wn9_data", "wn9_index"},
        )
        for entry in manifest["wakeword"]["files"]:
            self.assertEqual(set(entry), {"name", "sha256"})
        self.assertEqual(
            set(manifest["emoji_source"]), {"repository", "commit", "license"}
        )
        self.assertEqual(
            set(manifest["text_font"]),
            {
                "component",
                "version",
                "component_hash",
                "repository_commit",
                "license",
                "file",
                "sha256",
                "bundle",
                "charset",
                "size",
                "bpp",
            },
        )
        self.assertEqual(set(manifest["assets"]), {"file", "size_bytes", "sha256"})
        for entry in manifest["license_files"]:
            self.assertEqual(set(entry), {"asset_file", "sha256"})
        for entry in manifest["emoji_entries"]:
            self.assertEqual(
                set(entry), {"name", "file", "source_sha256", "sha256"}
            )

    def test_assets_doc_pins_sources_licenses_and_rollback(self) -> None:
        assets_doc = (BOARD_DIR / "ASSETS.md").read_text(encoding="utf-8")
        for required in (
            "55517b40d724014faff00f941ca700cbf9d14b51",
            "970cf66906d7c30059faa2704e7002f06b8c3619",
            "3f4f9fa5dcb703208bf22e46a185bb2b4cc9d002b9474b90f7c77e55e0ac541a",
            "d45dbc64052d57048f20ab1770074172ce9eb53b",
            "MIT",
            "Apache-2.0",
            "2f8c4b0459db5bbb39abd77adae27962d6d94bcb",
            "ESPRESSIF-MIT",
            "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n",
            "CONFIG_SR_WN_WN9_HIESP=n",
            "CONFIG_SR_WN_WN9_SOPHIA_TTS=n",
            "CONFIG_SR_WN_WN9_HEYILY_TTS2=y",
            "CONFIG_FLASH_CUSTOM_ASSETS=y",
            'CONFIG_CUSTOM_ASSETS_FILE="boards/irille-s3-eye/assets.bin"',
            "重刷 assets 分区",
        ):
            self.assertIn(required, assets_doc)

    def test_assets_doc_names_reproduction_audit_location(self) -> None:
        assets_doc = (BOARD_DIR / "ASSETS.md").read_text(encoding="utf-8")
        self.assertIn("## 可复现性审计", assets_doc)
        self.assertIn(
            "specs/003-wakeword-otto-persona/evidence/stage1.md", assets_doc
        )

    def test_rollback_flash_instructions_preserve_nvs(self) -> None:
        assets_doc = (BOARD_DIR / "ASSETS.md").read_text(encoding="utf-8")
        self.assertIn("NVS (`0x9000`–`0xcfff`)", assets_doc)
        self.assertIn('write-flash "@flash_args"', assets_doc)
        self.assertIn("不得把 `merged-binary.bin` 从 `0x0`", assets_doc)

    def test_real_otto_license_is_present_and_canonical(self) -> None:
        license_text = (
            BOARD_DIR / "LICENSES" / "otto-emoji-gif-component.LICENSE"
        ).read_text(encoding="utf-8")
        self.assertTrue(license_text.startswith("MIT License\n"))
        self.assertIn("Permission is hereby granted", license_text)


if __name__ == "__main__":
    unittest.main()
