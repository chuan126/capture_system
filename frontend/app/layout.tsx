import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  metadataBase: new URL(
    process.env.NEXT_PUBLIC_SITE_URL ?? "http://capture-system.local",
  ),
  title: "交科净界-大件运输净空动态分析系统",
  description: "交科净界-大件运输净空动态分析系统",
  openGraph: {
    title: "交科净界-大件运输净空动态分析系统",
    description: "交科净界-大件运输净空动态分析系统",
    images: [{ url: "/og.png", width: 1731, height: 909 }],
  },
  twitter: {
    card: "summary_large_image",
    title: "交科净界-大件运输净空动态分析系统",
    description: "交科净界-大件运输净空动态分析系统",
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
