import type { NextConfig } from "next";

const isDeviceStaticExport = process.env.CAPTURE_DEVICE_EXPORT === "1";

const nextConfig: NextConfig = {
  ...(isDeviceStaticExport ? { output: "export" as const } : {}),
};

export default nextConfig;
