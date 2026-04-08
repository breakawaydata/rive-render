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
  }
);
