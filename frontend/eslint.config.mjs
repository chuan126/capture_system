import { defineConfig, globalIgnores } from "eslint/config";
import nextVitals from "eslint-config-next/core-web-vitals";
import nextTs from "eslint-config-next/typescript";

const eslintConfig = defineConfig([
  ...nextVitals,
  ...nextTs,
  {
    rules: {
      // 实时数据通过WebSocket、轮询和外部地图SDK进入React，effect必须同步这些外部状态。
      "react-hooks/set-state-in-effect": "off",
      // 高频RTK轨迹由独立revision触发重绘，ref用于避免每个采样点复制整个轨迹数组。
      "react-hooks/refs": "off",
    },
  },
  // Override default ignores of eslint-config-next.
  globalIgnores([
    // Default ignores of eslint-config-next:
    ".next/**",
    "out/**",
    "build/**",
    "next-env.d.ts",
  ]),
]);

export default eslintConfig;
