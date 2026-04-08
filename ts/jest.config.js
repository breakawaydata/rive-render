const path = require("path");

const MOLTENVK_DIR = path.resolve(
  __dirname,
  "..",
  "deps",
  "rive-runtime",
  "renderer",
  "dependencies",
  "MoltenVK",
  "Package",
  "Release",
  "MoltenVK",
  "dylib",
  "macOS"
);

// Set DYLD_LIBRARY_PATH so spawned rive_render can find MoltenVK
process.env.DYLD_LIBRARY_PATH = [
  MOLTENVK_DIR,
  process.env.DYLD_LIBRARY_PATH,
]
  .filter(Boolean)
  .join(":");

/** @type {import('jest').Config} */
module.exports = {
  preset: "ts-jest",
  transform: {
    "^.+\\.ts$": [
      "ts-jest",
      { diagnostics: { ignoreCodes: [151002] } },
    ],
  },
  testEnvironment: "node",
  roots: ["<rootDir>/src"],
  testMatch: ["**/*.test.ts"],
  moduleNameMapper: {
    "^(\\.{1,2}/.*)\\.js$": "$1",
  },
  testTimeout: 30000,
};
