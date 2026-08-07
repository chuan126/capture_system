import { access } from "node:fs/promises";
import { fileURLToPath } from "node:url";

async function exists(url) {
  try {
    await access(fileURLToPath(url));
    return true;
  } catch {
    return false;
  }
}

export async function resolve(specifier, context, nextResolve) {
  try {
    return await nextResolve(specifier, context);
  } catch (error) {
    if (error?.code !== "ERR_MODULE_NOT_FOUND" || !specifier.startsWith(".")) {
      throw error;
    }

    const base = new URL(specifier, context.parentURL);
    for (const extension of [".ts", ".tsx"]) {
      const candidate = new URL(`${base.href}${extension}`);
      if (await exists(candidate)) {
        return nextResolve(candidate.href, context);
      }
    }

    throw error;
  }
}
