import eslint from "@eslint/js";
import tseslint from "typescript-eslint";

export default tseslint.config(
  eslint.configs.recommended,
  ...tseslint.configs.recommended,
  {
    ignores: ["dist/", "jest.config.js", "eslint.config.mjs"],
  },
  {
    files: ["src/test/**/*.ts"],
    rules: {
      "@typescript-eslint/no-require-imports": "off",
    },
  },
  {
    // Node.js CommonJS scripts (postinstall, etc.)
    files: ["scripts/**/*.js"],
    languageOptions: {
      sourceType: "commonjs",
      globals: {
        __dirname: "readonly",
        __filename: "readonly",
        Buffer: "readonly",
        console: "readonly",
        exports: "writable",
        global: "readonly",
        module: "writable",
        process: "readonly",
        require: "readonly",
        URL: "readonly",
      },
    },
    rules: {
      "@typescript-eslint/no-require-imports": "off",
    },
  }
);
