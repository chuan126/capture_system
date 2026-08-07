import { register } from "node:module";

register(new URL("./ts-extension-loader.mjs", import.meta.url), import.meta.url);
