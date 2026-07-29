import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "隧道净空测量显控终端",
  description: "Odin1 Lite 车载隧道净空高度测量显控界面",
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
