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


def _fixed_string(value: str, size: int = 32) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) > size:
        raise ValueError(value)
    return encoded.ljust(size, b"\0")


def _build_srmodels(model_names: list[str]) -> bytes:
    file_name = "_MODEL_INFO_"
    payloads = [f"model:{name}".encode() for name in model_names]
    header_size = 4 + len(model_names) * (32 + 4 + 32 + 4 + 4)
    headers = bytearray(struct.pack("<I", len(model_names)))
    data = bytearray()
    for model_name, payload in zip(model_names, payloads, strict=True):
        headers.extend(_fixed_string(model_name))
        headers.extend(struct.pack("<I", 1))
        headers.extend(_fixed_string(file_name))
        headers.extend(struct.pack("<II", header_size + len(data), len(payload)))
        data.extend(payload)
    return bytes(headers + data)


def _build_assets(files: dict[str, bytes]) -> bytes:
    table = bytearray()
    merged = bytearray()
    for file_name, payload in files.items():
        offset = len(merged)
        merged.extend(b"ZZ")
        merged.extend(payload)
        table.extend(_fixed_string(file_name))
        table.extend(struct.pack("<IIHH", len(payload), offset, 0, 0))
    combined = table + merged
    checksum = sum(combined) & 0xFFFF
    return struct.pack("<III", len(files), checksum, len(combined)) + combined


def _write_bundle(root: Path, model_names: list[str]) -> Path:
    font_payload = b"synthetic common font"
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
        "srmodels.bin": _build_srmodels(model_names),
        "font_noto_sans_common_16_4.bin": font_payload,
        **{f"{name}.gif": payload for name, payload in emoji_payloads.items()},
    }
    assets = _build_assets(files)
    assets_path = root / "assets.bin"
    assets_path.write_bytes(assets)
    (root / "LICENSES").mkdir()
    (root / "LICENSES" / "otto-emoji-gif-component.LICENSE").write_text(
        "MIT License\n", encoding="utf-8"
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
        "wakeword": {"display": "Hi ESP", "model": "wn9_hiesp"},
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
                "source_sha256": hashlib.sha256(payload).hexdigest(),
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
            for name, payload in sorted(emoji_payloads.items())
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


class AssetsValidatorTests(unittest.TestCase):
    def test_accepts_valid_synthetic_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(Path(temp), ["wn9_hiesp"])
            result = _run_validator(manifest)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_a_second_wakenet_model(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            manifest = _write_bundle(
                Path(temp), ["wn9_hiesp", "wn9_nihaoxiaozhi_tts"]
            )
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("exactly one WakeNet model", result.stderr)

    def test_rejects_corrupt_assets_checksum(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_hiesp"])
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

    def test_rejects_wrong_font_repository_commit(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = _write_bundle(root, ["wn9_hiesp"])
            data = json.loads(manifest.read_text(encoding="utf-8"))
            data["text_font"]["repository_commit"] = "0" * 40
            manifest.write_text(json.dumps(data), encoding="utf-8")
            result = _run_validator(manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("text font repository commit mismatch", result.stderr)


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

    def test_board_selects_only_hi_esp_custom_assets(self) -> None:
        config = json.loads((BOARD_DIR / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(len(config["builds"]), 1)
        sdkconfig = set(config["builds"][0]["sdkconfig_append"])
        self.assertIn("CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n", sdkconfig)
        self.assertIn("CONFIG_SR_WN_WN9_HIESP=y", sdkconfig)
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
                "assets",
            },
        )
        self.assertEqual(set(manifest["generator"]), {"repository", "commit"})
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
            "CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n",
            "CONFIG_SR_WN_WN9_HIESP=y",
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
