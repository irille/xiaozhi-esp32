#!/usr/bin/env python3
"""校验 irille-s3-eye 自定义 assets.bin 与来源清单。"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any


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
EXPECTED_GENERATOR_REPOSITORY = "https://github.com/78/xiaozhi-assets-generator"
EXPECTED_GENERATOR_COMMIT = "55517b40d724014faff00f941ca700cbf9d14b51"
EXPECTED_EMOJI_REPOSITORY = (
    "https://github.com/txp666/otto-emoji-gif-component"
)
EXPECTED_EMOJI_COMMIT = "970cf66906d7c30059faa2704e7002f06b8c3619"
EXPECTED_OTTO_LICENSE_SHA256 = (
    "bd806361232a065ead834a53a04b34ba51eacb257ccdb21a6506f0e8738930d8"
)
EXPECTED_LICENSE_FILES = {
    "LICENSE.otto-emoji-gif.txt": (
        "otto-emoji-gif-component.LICENSE",
        EXPECTED_OTTO_LICENSE_SHA256,
    ),
    "LICENSE.xiaozhi-fonts.txt": (
        "xiaozhi-fonts.Apache-2.0.LICENSE",
        "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
    ),
    "LICENSE.esp-sr.txt": (
        "esp-sr.ESPRESSIF-MIT.LICENSE",
        "4216dce10853a02d02f815e21f10a72f51de610f45fb995108f6dbada595ef70",
    ),
}
EXPECTED_EMOJI_SOURCE_SHA256 = {
    "neutral": "edcb042a7ff48dcc0da0d7bafa7a08171e41ad9efe29d2bc5d552f0ed8b27eb7",
    "happy": "9d2c6701d7a7a81208a00cf4c76ec1ca64f1f6ddc4a884b94aa489cc71c120a3",
    "laughing": "2e571fdc66c48db6a00de204578bbe6eda71ae6a21953981cbc68be3ba733996",
    "funny": "b3020524fcea2bcdb311bd84b0e6aa87000fbf4caf5f512e4918acc506bf8808",
    "sad": "6fc08f07bc2275479ed692c0ae5f8c0fce761925696129fc233645df895b5cfb",
    "angry": "d67aeb6d7a8c5678cbc472757bf5bd5565967d70fc46e780a9ae260c738679b1",
    "crying": "cad138dec2d92292df6d1d053375484d620e9e1f51e64c5b2a4f1d1d338bcd14",
    "loving": "3c466f82ceca7c239ea8d27a3fcb6091b63b179234c8350334fa074c0a0d7ff1",
    "embarrassed": "d9ab84a9839067e935d9db5811a0b113545f4b486ca1384a38353bb37312c915",
    "surprised": "36e782c38f3f714cdcc7d61e008d77645885115c72eee04cb9e2ffa31fa333f1",
    "shocked": "45141d5822624a890bf692842695589ac7a2c6126079dcbec45d746938640c43",
    "thinking": "105d1495832a48b17cc97782666ed6ccd766bbdbfce187f49ff17929f97f5b6c",
    "winking": "6ed0d8757fdf6bca59e5851ea7f968b6df13f2abf6fffb0f4ba512a09e68bfd2",
    "cool": "f332bd852f4d1ea7cc2b390626812ec0ce8171899c88bd71dd08dcc56f2bbc05",
    "relaxed": "ce22ef9b70f4b6602c8389c1f3080e6a479e5d7e7bd916d3c94da71ce33ae39e",
    "delicious": "23607f905f87b42ae40fe6c20e6e27ba3acf4700c803686606a8cb60b9e2eb93",
    "kissy": "5a5f9067bc21e71813d0c9c6a0c17289dfc31f35459483e4ad131a3e1e029403",
    "confident": "2f498822df14c6d4654d6d4a9db87ba9ce55b625bea7484dad07164bbe0e15e9",
    "sleepy": "6a3f0622163ceec2a3698b77a644b3fe6e227b81432ec49c3373bfcc3d1b035c",
    "silly": "9a1573f1dabe504d283c5f3ae9a5be2fbeba8355b6a644917551bf086165ba8b",
    "confused": "244a67c812f8b6a39681258a16f38249f7c47d496ed77c7731ab6d5069ac0fab",
}
EXPECTED_WAKEWORD_MODEL = "wn9_heyily_tts2"
EXPECTED_WAKEWORD_COMPONENT = "espressif/esp-sr"
EXPECTED_WAKEWORD_VERSION = "2.4.7"
EXPECTED_WAKEWORD_COMPONENT_HASH = (
    "809d0041cdddd98a278f0d5afef7bb60a451290577b98cf718dfffc91bdcbd9b"
)
EXPECTED_WAKEWORD_REPOSITORY_COMMIT = "2f8c4b0459db5bbb39abd77adae27962d6d94bcb"
EXPECTED_WAKEWORD_LICENSE = "ESPRESSIF-MIT"
EXPECTED_WAKEWORD_FILES_SHA256 = {
    "_MODEL_INFO_": "dc4db6b880e0d9511575e93c13e2bc61167076650c4ffdd576c82d59fa67a4e8",
    "wn9_data": "3ea182aa12253d6acd3e8c5c48037150568b0f9fcfb8a5bbfc614fce4b25f4a2",
    "wn9_index": "f13338e279d66ecbac972424a3be4c6184708e6019b9aae613243e83bc6230f9",
}
EXPECTED_CUSTOM_PHRASES = {"你好爱莉丝", "Hi Iris"}
EXPECTED_CUSTOM_MODEL_FAMILY = "WakeNet9"
EXPECTED_CUSTOM_MODEL_FORMAT = "srmodels-v1"
EXPECTED_CUSTOM_LICENSE = "non-exclusive-commercial"
EXPECTED_CUSTOM_TARGET = "esp32s3"
EXPECTED_CUSTOM_IDF_VERSION = "6.0.2"
EXPECTED_CUSTOM_ESP_SR_VERSION = "2.4.7"
EXPECTED_FONT_COMPONENT = "78/xiaozhi-fonts"
EXPECTED_FONT_VERSION = "2.0.0"
EXPECTED_FONT_COMPONENT_HASH = (
    "3f4f9fa5dcb703208bf22e46a185bb2b4cc9d002b9474b90f7c77e55e0ac541a"
)
EXPECTED_FONT_REPOSITORY_COMMIT = "d45dbc64052d57048f20ab1770074172ce9eb53b"
EXPECTED_FONT_FILE = "font_noto_sans_common_16_4.bin"
EXPECTED_FONT_SHA256 = (
    "6c801b34ec686e6e31223eceedea2efe5b0cf294b3556d91087a683c86a54384"
)
EXPECTED_FONT_META = {
    "bundle": "noto-v1",
    "charset": "common",
    "size": 16,
    "bpp": 4,
}
ASSET_HEADER_SIZE = 12
ASSET_ENTRY_SIZE = 44
ASSET_NAME_SIZE = 32
MODEL_NAME_SIZE = 32
MODEL_FILE_ENTRY_SIZE = 40
DATA_PREFIX = b"ZZ"
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
COMMIT_PATTERN = re.compile(r"^[0-9a-f]{40}$")


class ValidationError(ValueError):
    """资产合同校验失败。"""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _decode_fixed_string(payload: bytes, field: str) -> str:
    raw = payload.split(b"\0", 1)[0]
    _require(bool(raw), f"{field} is empty")
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValidationError(f"{field} is not valid UTF-8") from error


def _resolve_child(root: Path, relative: str, field: str) -> Path:
    candidate = Path(relative)
    _require(not candidate.is_absolute(), f"{field} must be relative")
    resolved_root = root.resolve()
    resolved = (root / candidate).resolve()
    _require(
        resolved == resolved_root or resolved_root in resolved.parents,
        f"{field} escapes the board directory",
    )
    return resolved


def _parse_assets(payload: bytes) -> dict[str, bytes]:
    _require(len(payload) >= ASSET_HEADER_SIZE, "assets header is truncated")
    file_count, checksum, combined_size = struct.unpack_from("<III", payload)
    _require(file_count > 0, "assets file table is empty")
    _require(
        len(payload) == ASSET_HEADER_SIZE + combined_size,
        "assets combined length does not match header",
    )
    combined = payload[ASSET_HEADER_SIZE:]
    _require(
        sum(combined) & 0xFFFF == checksum,
        "assets checksum does not match header",
    )
    table_size = file_count * ASSET_ENTRY_SIZE
    _require(table_size <= len(combined), "assets file table is truncated")
    table = combined[:table_size]
    data = combined[table_size:]
    files: dict[str, bytes] = {}
    ranges: list[tuple[int, int, str]] = []

    for index in range(file_count):
        entry_offset = index * ASSET_ENTRY_SIZE
        name = _decode_fixed_string(
            table[entry_offset : entry_offset + ASSET_NAME_SIZE],
            f"asset entry {index} name",
        )
        size, offset, _width, _height = struct.unpack_from(
            "<IIHH", table, entry_offset + ASSET_NAME_SIZE
        )
        _require(name not in files, f"duplicate asset filename: {name}")
        _require(offset + len(DATA_PREFIX) + size <= len(data), f"{name} is out of bounds")
        _require(
            data[offset : offset + len(DATA_PREFIX)] == DATA_PREFIX,
            f"{name} is missing 0x5A5A data prefix",
        )
        start = offset + len(DATA_PREFIX)
        end = start + size
        files[name] = data[start:end]
        ranges.append((offset, end, name))

    ranges.sort()
    _require(ranges[0][0] == 0, "assets data starts with unreferenced bytes")
    for previous, current in zip(ranges, ranges[1:], strict=False):
        _require(
            previous[1] == current[0],
            f"assets data contains gaps or overlapping payloads: {previous[2]} and {current[2]}",
        )
    _require(ranges[-1][1] == len(data), "assets data ends with unreferenced bytes")
    return files


def _parse_models(payload: bytes) -> tuple[list[str], dict[str, dict[str, bytes]]]:
    _require(len(payload) >= 4, "srmodels header is truncated")
    model_count = struct.unpack_from("<I", payload)[0]
    _require(model_count > 0, "srmodels contains no models")
    cursor = 4
    model_names: list[str] = []
    model_files: dict[str, dict[str, bytes]] = {}
    file_ranges: list[tuple[int, int, str]] = []

    for model_index in range(model_count):
        _require(cursor + MODEL_NAME_SIZE + 4 <= len(payload), "srmodels model header is truncated")
        model_name = _decode_fixed_string(
            payload[cursor : cursor + MODEL_NAME_SIZE],
            f"srmodels model {model_index} name",
        )
        cursor += MODEL_NAME_SIZE
        file_count = struct.unpack_from("<I", payload, cursor)[0]
        cursor += 4
        _require(file_count > 0, f"WakeNet model {model_name} has no files")
        model_names.append(model_name)
        files: dict[str, bytes] = {}
        for file_index in range(file_count):
            _require(
                cursor + MODEL_FILE_ENTRY_SIZE <= len(payload),
                f"srmodels file table for {model_name} is truncated",
            )
            file_name = _decode_fixed_string(
                payload[cursor : cursor + MODEL_NAME_SIZE],
                f"srmodels {model_name} file {file_index}",
            )
            start, size = struct.unpack_from("<II", payload, cursor + MODEL_NAME_SIZE)
            cursor += MODEL_FILE_ENTRY_SIZE
            _require(start + size <= len(payload), f"{model_name}/{file_name} is out of bounds")
            _require(file_name not in files, f"duplicate WakeNet model file: {model_name}/{file_name}")
            files[file_name] = payload[start : start + size]
            file_ranges.append((start, start + size, f"{model_name}/{file_name}"))
        model_files[model_name] = files

    _require(len(set(model_names)) == len(model_names), "duplicate WakeNet model name")
    file_ranges.sort()
    for start, end, name in file_ranges:
        _require(start >= cursor, f"{name} overlaps srmodels header")
        _require(end > start, f"{name} is empty")
    _require(file_ranges[0][0] == cursor, "srmodels data starts with unreferenced bytes")
    for previous, current in zip(file_ranges, file_ranges[1:], strict=False):
        _require(
            previous[1] == current[0],
            f"srmodels data contains gaps or overlapping payloads: {previous[2]} and {current[2]}",
        )
    _require(file_ranges[-1][1] == len(payload), "srmodels data ends with unreferenced bytes")
    return model_names, model_files


def _load_manifest(path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValidationError(f"cannot read manifest: {error}") from error
    _require(isinstance(manifest, dict), "manifest root must be an object")
    return manifest


def validate_candidate_model(candidate_path: Path) -> dict[str, Any]:
    """校验阶段二供应商候选模型元数据，不改变阶段一发布资产合同。"""
    candidate = _load_manifest(candidate_path)
    candidate_id = candidate.get("candidate_id")
    _require(isinstance(candidate_id, str) and candidate_id, "candidate_id is required")
    phrases = candidate.get("phrases")
    _require(
        isinstance(phrases, list)
        and len(phrases) == len(EXPECTED_CUSTOM_PHRASES)
        and all(isinstance(phrase, str) for phrase in phrases)
        and set(phrases) == EXPECTED_CUSTOM_PHRASES,
        "candidate phrases must be exactly 你好爱莉丝 and Hi Iris",
    )

    source = candidate.get("source")
    _require(isinstance(source, dict), "candidate source must be an object")
    _require(
        isinstance(source.get("supplier"), str) and bool(source["supplier"]),
        "candidate supplier is required",
    )
    _require(
        source.get("license") == EXPECTED_CUSTOM_LICENSE,
        "candidate requires a non-exclusive commercial license",
    )

    compatibility = candidate.get("compatibility")
    _require(isinstance(compatibility, dict), "compatibility must be an object")
    _require(
        compatibility.get("target") == EXPECTED_CUSTOM_TARGET,
        f"candidate target must be {EXPECTED_CUSTOM_TARGET}",
    )
    _require(
        compatibility.get("esp_idf") == EXPECTED_CUSTOM_IDF_VERSION
        and compatibility.get("esp_sr") == EXPECTED_CUSTOM_ESP_SR_VERSION,
        "candidate must support ESP-IDF 6.0.2 and ESP-SR 2.4.7",
    )

    models = candidate.get("models")
    _require(
        isinstance(models, list) and 1 <= len(models) <= len(EXPECTED_CUSTOM_PHRASES),
        "candidate must contain one dual-keyword model or two separate models",
    )
    model_names: list[str] = []
    for model in models:
        _require(isinstance(model, dict), "candidate model must be an object")
        model_name = model.get("model_name")
        _require(isinstance(model_name, str) and model_name, "candidate model_name is required")
        model_names.append(model_name)
        _require(
            model.get("model_family") == EXPECTED_CUSTOM_MODEL_FAMILY
            and model.get("format") == EXPECTED_CUSTOM_MODEL_FORMAT,
            "candidate model must use WakeNet9 srmodels-v1 format",
        )
    _require(len(set(model_names)) == len(model_names), "candidate model names must be unique")

    if len(models) == 1:
        _require(
            models[0].get("delivery_shape") == "multi-keyword",
            "a single custom model must use multi-keyword delivery",
        )
    else:
        proof = candidate.get("wrapper_proof")
        _require(
            isinstance(proof, dict)
            and proof.get("mapping_validated") is True
            and proof.get("resources_validated") is True,
            "separate models require wrapper mapping and resource proof",
        )
        _require(
            all(model.get("delivery_shape") == "separate-model" for model in models),
            "multiple custom models must use separate-model delivery",
        )

    return {
        "candidate_id": candidate_id,
        "phrases": phrases,
        "model_count": len(models),
        "delivery_shape": models[0].get("delivery_shape"),
        "format": EXPECTED_CUSTOM_MODEL_FORMAT,
        "license": EXPECTED_CUSTOM_LICENSE,
    }


def validate(manifest_path: Path) -> dict[str, Any]:
    manifest = _load_manifest(manifest_path)
    root = manifest_path.parent

    _require(manifest.get("schema_version") == 1, "schema_version must be 1")
    _require(manifest.get("target_board") == "irille-s3-eye", "target_board mismatch")
    partition_limit = manifest.get("partition_limit_bytes")
    _require(partition_limit == 0x200000, "partition_limit_bytes must be 0x200000")

    generator = manifest.get("generator")
    _require(isinstance(generator, dict), "generator must be an object")
    _require(
        generator.get("repository") == EXPECTED_GENERATOR_REPOSITORY,
        "generator repository mismatch",
    )
    generator_commit = generator.get("commit", "")
    _require(
        isinstance(generator_commit, str)
        and COMMIT_PATTERN.fullmatch(generator_commit) is not None,
        "generator commit must be a 40-character SHA",
    )
    _require(generator_commit == EXPECTED_GENERATOR_COMMIT, "generator commit mismatch")

    emoji_source = manifest.get("emoji_source")
    _require(isinstance(emoji_source, dict), "emoji_source must be an object")
    _require(
        emoji_source.get("repository") == EXPECTED_EMOJI_REPOSITORY,
        "emoji source repository mismatch",
    )
    emoji_commit = emoji_source.get("commit", "")
    _require(
        isinstance(emoji_commit, str)
        and COMMIT_PATTERN.fullmatch(emoji_commit) is not None,
        "emoji source commit must be a 40-character SHA",
    )
    _require(emoji_commit == EXPECTED_EMOJI_COMMIT, "emoji source commit mismatch")
    _require(emoji_source.get("license") == "MIT", "emoji source license must be MIT")

    text_font = manifest.get("text_font")
    _require(isinstance(text_font, dict), "text_font must be an object")
    _require(text_font.get("component") == EXPECTED_FONT_COMPONENT, "text font component mismatch")
    _require(text_font.get("version") == EXPECTED_FONT_VERSION, "text font version mismatch")
    _require(
        text_font.get("component_hash") == EXPECTED_FONT_COMPONENT_HASH,
        "text font component hash mismatch",
    )
    _require(
        text_font.get("repository_commit") == EXPECTED_FONT_REPOSITORY_COMMIT,
        "text font repository commit mismatch",
    )
    _require(text_font.get("license") == "Apache-2.0", "text font license mismatch")
    _require(text_font.get("file") == EXPECTED_FONT_FILE, "text font file mismatch")
    for field, expected in EXPECTED_FONT_META.items():
        _require(text_font.get(field) == expected, f"text font {field} mismatch")
    font_sha = text_font.get("sha256")
    _require(font_sha == EXPECTED_FONT_SHA256, "text font SHA-256 mismatch")

    wakeword = manifest.get("wakeword")
    _require(isinstance(wakeword, dict), "wakeword must be an object")
    _require(wakeword.get("display") == "Hey,Ily", "wakeword display must be Hey,Ily")
    _require(
        wakeword.get("model") == EXPECTED_WAKEWORD_MODEL,
        f"wakeword model must be {EXPECTED_WAKEWORD_MODEL}",
    )
    _require(
        wakeword.get("component") == EXPECTED_WAKEWORD_COMPONENT,
        "wakeword component mismatch",
    )
    _require(
        wakeword.get("version") == EXPECTED_WAKEWORD_VERSION,
        "wakeword component version mismatch",
    )
    _require(
        wakeword.get("component_hash") == EXPECTED_WAKEWORD_COMPONENT_HASH,
        "wakeword component hash mismatch",
    )
    _require(
        wakeword.get("repository_commit") == EXPECTED_WAKEWORD_REPOSITORY_COMMIT,
        "wakeword repository commit mismatch",
    )
    _require(
        wakeword.get("license") == EXPECTED_WAKEWORD_LICENSE,
        "wakeword license mismatch",
    )
    expected_wakeword_manifest = [
        {"name": name, "sha256": digest}
        for name, digest in EXPECTED_WAKEWORD_FILES_SHA256.items()
    ]
    _require(
        wakeword.get("files") == expected_wakeword_manifest,
        "wakeword files do not match the pinned ESP-SR model payloads",
    )

    for _asset_file, (source_file, expected_sha) in EXPECTED_LICENSE_FILES.items():
        license_path = root / "LICENSES" / source_file
        _require(license_path.is_file(), f"license file is missing: {source_file}")
        _require(
            _sha256(license_path.read_bytes()) == expected_sha,
            f"license SHA-256 mismatch: {source_file}",
        )

    license_files = manifest.get("license_files")
    _require(isinstance(license_files, list), "license_files must be a list")
    expected_license_manifest = [
        {"asset_file": asset_file, "sha256": expected_sha}
        for asset_file, (_source_file, expected_sha) in EXPECTED_LICENSE_FILES.items()
    ]
    _require(
        license_files == expected_license_manifest,
        "license_files do not match the pinned release licenses",
    )

    assets_meta = manifest.get("assets")
    _require(isinstance(assets_meta, dict), "assets must be an object")
    assets_file = assets_meta.get("file")
    _require(assets_file == "assets.bin", "assets.file must be assets.bin")
    assets_path = _resolve_child(root, assets_file, "assets.file")
    _require(assets_path.is_file(), f"assets file is missing: {assets_file}")
    assets_payload = assets_path.read_bytes()
    _require(
        assets_meta.get("size_bytes") == len(assets_payload),
        "assets size does not match manifest",
    )
    _require(0 < len(assets_payload) <= partition_limit, "assets exceeds partition limit")
    assets_sha = assets_meta.get("sha256")
    _require(
        isinstance(assets_sha, str) and SHA256_PATTERN.fullmatch(assets_sha) is not None,
        "assets sha256 must be 64 lowercase hex characters",
    )
    _require(_sha256(assets_payload) == assets_sha, "assets sha256 does not match manifest")

    files = _parse_assets(assets_payload)
    _require("index.json" in files, "assets is missing index.json")
    _require("srmodels.bin" in files, "assets is missing srmodels.bin")
    try:
        index = json.loads(files["index.json"].decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValidationError(f"index.json is invalid: {error}") from error
    _require(index.get("version") == 1, "index.json version must be 1")
    _require(index.get("srmodels") == "srmodels.bin", "index.json srmodels mismatch")
    _require(index.get("text_font") == EXPECTED_FONT_FILE, "index.json text_font mismatch")
    _require(index.get("text_font_meta") == EXPECTED_FONT_META, "index.json text_font_meta mismatch")
    _require("multinet" not in index and "multinet_model" not in index, "index.json must not include MultiNet")
    for skin in (index.get("skin") or {}).values():
        if isinstance(skin, dict):
            _require(not skin.get("background_image"), "index.json must not include a background image")

    emoji_entries = manifest.get("emoji_entries")
    _require(isinstance(emoji_entries, list), "emoji_entries must be a list")
    _require(len(emoji_entries) == len(EXPECTED_EMOTIONS), "manifest must contain 21 emoji entries")
    manifest_by_name: dict[str, dict[str, Any]] = {}
    for entry in emoji_entries:
        _require(isinstance(entry, dict), "emoji entry must be an object")
        name = entry.get("name")
        file_name = entry.get("file")
        digest = entry.get("sha256")
        source_digest = entry.get("source_sha256")
        _require(isinstance(name, str) and name, "emoji name is required")
        _require(name not in manifest_by_name, f"duplicate emoji name: {name}")
        _require(file_name == f"{name}.gif", f"emoji file mismatch for {name}")
        _require(
            isinstance(digest, str) and SHA256_PATTERN.fullmatch(digest) is not None,
            f"emoji sha256 is invalid for {name}",
        )
        _require(
            isinstance(source_digest, str)
            and SHA256_PATTERN.fullmatch(source_digest) is not None,
            f"emoji source_sha256 is invalid for {name}",
        )
        _require(
            source_digest == EXPECTED_EMOJI_SOURCE_SHA256.get(name),
            f"emoji source_sha256 mismatch for {name}",
        )
        manifest_by_name[name] = entry
    _require(set(manifest_by_name) == EXPECTED_EMOTIONS, "manifest emoji names do not match the canonical set")

    index_entries = index.get("emoji_collection")
    _require(isinstance(index_entries, list), "index.json emoji_collection must be a list")
    index_pairs = [(entry.get("name"), entry.get("file")) for entry in index_entries if isinstance(entry, dict)]
    _require(len(index_pairs) == len(EXPECTED_EMOTIONS), "index.json must contain 21 emoji entries")
    _require(len(set(index_pairs)) == len(index_pairs), "index.json contains duplicate emoji entries")
    _require(
        set(index_pairs) == {(name, f"{name}.gif") for name in EXPECTED_EMOTIONS},
        "index.json emoji entries do not match the canonical set",
    )

    expected_files = {
        "index.json",
        "srmodels.bin",
        EXPECTED_FONT_FILE,
        *EXPECTED_LICENSE_FILES,
    } | {
        f"{name}.gif" for name in EXPECTED_EMOTIONS
    }
    _require(set(files) == expected_files, "assets contains missing or unexpected files")
    _require(_sha256(files[EXPECTED_FONT_FILE]) == font_sha, "text font sha256 mismatch")
    for asset_file, (_source_file, expected_sha) in EXPECTED_LICENSE_FILES.items():
        _require(
            _sha256(files[asset_file]) == expected_sha,
            f"embedded license SHA-256 mismatch: {asset_file}",
        )
    for name, entry in manifest_by_name.items():
        payload = files[entry["file"]]
        _require(payload.startswith((b"GIF87a", b"GIF89a")), f"{name} is not a GIF")
        _require(_sha256(payload) == entry["sha256"], f"emoji sha256 mismatch for {name}")

    model_names, model_files = _parse_models(files["srmodels.bin"])
    _require(
        model_names == [EXPECTED_WAKEWORD_MODEL],
        f"expected exactly one WakeNet model ({EXPECTED_WAKEWORD_MODEL}); found {model_names}",
    )
    wakeword_files = model_files[EXPECTED_WAKEWORD_MODEL]
    _require(
        set(wakeword_files) == set(EXPECTED_WAKEWORD_FILES_SHA256),
        "WakeNet model files do not match the pinned ESP-SR payload set",
    )
    for file_name, expected_sha in EXPECTED_WAKEWORD_FILES_SHA256.items():
        _require(
            _sha256(wakeword_files[file_name]) == expected_sha,
            f"WakeNet payload SHA-256 mismatch: {file_name}",
        )

    return {
        "bundle_id": manifest.get("bundle_id"),
        "assets_file": str(assets_path),
        "size_bytes": len(assets_payload),
        "sha256": assets_sha,
        "emoji_count": len(index_pairs),
        "wakeword_models": model_names,
        "text_font": EXPECTED_FONT_META,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, nargs="?")
    parser.add_argument("--candidate-model", type=Path)
    args = parser.parse_args()
    if args.manifest is None and args.candidate_model is None:
        parser.error("provide an assets manifest or --candidate-model")
    if args.manifest is not None and args.candidate_model is not None:
        parser.error("assets manifest and --candidate-model are mutually exclusive")
    try:
        if args.candidate_model is not None:
            summary = validate_candidate_model(args.candidate_model)
        else:
            summary = validate(args.manifest)
    except (OSError, ValidationError) as error:
        print(f"asset validation failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
