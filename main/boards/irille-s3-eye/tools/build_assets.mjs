#!/usr/bin/env node
/**
 * 用固定 xiaozhi-assets-generator 模块打包 Hey,Ily + Otto assets.bin。
 *
 * 输入仓库必须已 checkout 到本文件内声明的精确 commit。
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { fileURLToPath, pathToFileURL } from "node:url";

const GENERATOR_COMMIT = "55517b40d724014faff00f941ca700cbf9d14b51";
const OTTO_COMMIT = "970cf66906d7c30059faa2704e7002f06b8c3619";
const FONT_COMPONENT_HASH =
  "3f4f9fa5dcb703208bf22e46a185bb2b4cc9d002b9474b90f7c77e55e0ac541a";
const FONT_FILE = "font_noto_sans_common_16_4.bin";
const FONT_SHA256 =
  "6c801b34ec686e6e31223eceedea2efe5b0cf294b3556d91087a683c86a54384";
const FFMPEG_VERSION = "8.1.2";
const MODEL_NAME = "wn9_heyily_tts2";
const LICENSE_FILES = [
  {
    source: "otto-emoji-gif-component.LICENSE",
    asset: "LICENSE.otto-emoji-gif.txt",
    sha256: "bd806361232a065ead834a53a04b34ba51eacb257ccdb21a6506f0e8738930d8",
  },
  {
    source: "xiaozhi-fonts.Apache-2.0.LICENSE",
    asset: "LICENSE.xiaozhi-fonts.txt",
    sha256: "c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4",
  },
  {
    source: "esp-sr.ESPRESSIF-MIT.LICENSE",
    asset: "LICENSE.esp-sr.txt",
    sha256: "4216dce10853a02d02f815e21f10a72f51de610f45fb995108f6dbada595ef70",
  },
];
const EMOTIONS = [
  "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
  "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
  "cool", "relaxed", "delicious", "kissy", "confident", "sleepy", "silly",
  "confused",
];

function usage() {
  console.error(
    "usage: build_assets.mjs <xiaozhi-assets-generator> <otto-component> " +
      "<xiaozhi-fonts-component> <output>",
  );
}

function assertSourceRepository(repositoryPath, expected) {
  const actual = execFileSync(
    "git",
    ["-C", repositoryPath, "rev-parse", "HEAD"],
    { encoding: "utf8" },
  ).trim();
  if (actual !== expected) {
    throw new Error(
      `unexpected source commit for ${repositoryPath}: ${actual}; expected ${expected}`,
    );
  }
  const status = execFileSync(
    "git",
    ["-C", repositoryPath, "status", "--porcelain=v1", "--untracked-files=all"],
    { encoding: "utf8" },
  ).trim();
  if (status) {
    throw new Error(`source repository is dirty: ${repositoryPath}`);
  }
}

function sha256(payload) {
  return createHash("sha256").update(payload).digest("hex");
}

const arguments_ = process.argv.slice(2);
if (arguments_[0] === "--check-repository") {
  if (!arguments_[1] || !arguments_[2] || arguments_.length !== 3) {
    console.error("usage: build_assets.mjs --check-repository <path> <commit>");
    process.exit(2);
  }
  assertSourceRepository(path.resolve(arguments_[1]), arguments_[2]);
  process.exit(0);
}

const [generatorRootArg, ottoRootArg, fontRootArg, outputArg] = arguments_;
if (!generatorRootArg || !ottoRootArg || !fontRootArg || !outputArg) {
  usage();
  process.exit(2);
}

const generatorRoot = path.resolve(generatorRootArg);
const ottoRoot = path.resolve(ottoRootArg);
const fontRoot = path.resolve(fontRootArg);
const outputPath = path.resolve(outputArg);
const boardRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
assertSourceRepository(generatorRoot, GENERATOR_COMMIT);
assertSourceRepository(ottoRoot, OTTO_COMMIT);

const componentHash = (
  await fs.readFile(path.join(fontRoot, ".component_hash"), "utf8")
).trim();
if (componentHash !== FONT_COMPONENT_HASH) {
  throw new Error(
    `unexpected xiaozhi-fonts component hash: ${componentHash}; ` +
      `expected ${FONT_COMPONENT_HASH}`,
  );
}
const fontPayload = await fs.readFile(path.join(fontRoot, "cbin", FONT_FILE));
if (sha256(fontPayload) !== FONT_SHA256) {
  throw new Error(`unexpected SHA-256 for ${FONT_FILE}`);
}
const ffmpegBanner = execFileSync("ffmpeg", ["-version"], {
  encoding: "utf8",
}).split("\n", 1)[0];
if (!ffmpegBanner.startsWith(`ffmpeg version ${FFMPEG_VERSION} `)) {
  throw new Error(
    `unexpected ffmpeg version: ${ffmpegBanner}; expected ${FFMPEG_VERSION}`,
  );
}

const { default: WakenetModelPacker } = await import(
  pathToFileURL(path.join(generatorRoot, "web/src/utils/WakenetModelPacker.js"))
);
const { default: SpiffsGenerator } = await import(
  pathToFileURL(path.join(generatorRoot, "web/src/utils/SpiffsGenerator.js"))
);

const modelDir = path.join(
  generatorRoot,
  "web/public/static/wakenet_model",
  MODEL_NAME,
);
const modelPacker = new WakenetModelPacker();
for (const fileName of ["_MODEL_INFO_", "wn9_data", "wn9_index"]) {
  const payload = await fs.readFile(path.join(modelDir, fileName));
  modelPacker.addModelFile(
    MODEL_NAME,
    fileName,
    payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength),
  );
}

const index = {
  version: 1,
  chip_model: "esp32s3",
  display_config: {
    width: 240,
    height: 240,
    monochrome: false,
    color: "RGB565",
  },
  srmodels: "srmodels.bin",
  text_font: FONT_FILE,
  text_font_meta: {
    bundle: "noto-v1",
    charset: "common",
    size: 16,
    bpp: 4,
  },
  emoji_collection: EMOTIONS.map((name) => ({
    name,
    file: `${name}.gif`,
  })),
};

const generator = new SpiffsGenerator();
const indexPayload = new TextEncoder().encode(JSON.stringify(index, null, 2));
generator.addFile(
  "index.json",
  indexPayload.buffer.slice(
    indexPayload.byteOffset,
    indexPayload.byteOffset + indexPayload.byteLength,
  ),
);
generator.addFile("srmodels.bin", modelPacker.packModels());
generator.addFile(
  FONT_FILE,
  fontPayload.buffer.slice(
    fontPayload.byteOffset,
    fontPayload.byteOffset + fontPayload.byteLength,
  ),
);
for (const license of LICENSE_FILES) {
  const payload = await fs.readFile(path.join(boardRoot, "LICENSES", license.source));
  if (sha256(payload) !== license.sha256) {
    throw new Error(`unexpected SHA-256 for ${license.source}`);
  }
  generator.addFile(
    license.asset,
    payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength),
  );
}

const optimizedRoot = await fs.mkdtemp(
  path.join(os.tmpdir(), "irille-s3-eye-assets-"),
);
try {
  for (const name of EMOTIONS) {
    const source = path.join(ottoRoot, "gifs", `${name}.gif`);
    const optimized = path.join(optimizedRoot, `${name}.gif`);
    execFileSync(
      "ffmpeg",
      [
        "-loglevel", "error", "-y", "-i", source,
        "-fps_mode", "passthrough", optimized,
      ],
      { stdio: "inherit" },
    );
    const payload = await fs.readFile(optimized);
    generator.addFile(
      `${name}.gif`,
      payload.buffer.slice(
        payload.byteOffset,
        payload.byteOffset + payload.byteLength,
      ),
      { width: 240, height: 240 },
    );
  }
} finally {
  await fs.rm(optimizedRoot, { recursive: true, force: true });
}

const assets = await generator.generate();
await fs.writeFile(outputPath, new Uint8Array(assets));
console.log(
  JSON.stringify({
    generatorCommit: GENERATOR_COMMIT,
    ottoCommit: OTTO_COMMIT,
    fontComponentHash: FONT_COMPONENT_HASH,
    ffmpegVersion: FFMPEG_VERSION,
    output: outputPath,
    fileCount: generator.getStats().fileCount,
    sizeBytes: assets.byteLength,
  }),
);
