import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  metadataBase: new URL(
    process.env.NEXT_PUBLIC_SITE_URL ?? "http://capture-system.local",
  ),
  title: "隧道净空测量显控终端",
  description: "Odin1 Lite 车载隧道净空高度测量显控界面",
  openGraph: {
    title: "车载隧道净空高度测量",
    description: "三维采集系统 · Odin1 Lite · RK3588",
    images: [{ url: "/og.png", width: 1731, height: 909 }],
  },
  twitter: {
    card: "summary_large_image",
    title: "车载隧道净空高度测量",
    description: "三维采集系统 · Odin1 Lite · RK3588",
    images: ["/og.png"],
  },
  icons: {
    icon: "/favicon.svg",
    shortcut: "/favicon.svg",
  },
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN">
      <body>{children}</body>
    </html>
  );
}
