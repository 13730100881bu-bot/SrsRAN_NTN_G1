import type { Metadata } from "next";
import { headers } from "next/headers";
import "./globals.css";

export async function generateMetadata(): Promise<Metadata> {
  const requestHeaders = await headers();
  const host = requestHeaders.get("x-forwarded-host") ?? requestHeaders.get("host") ?? "localhost:3001";
  const protocol = requestHeaders.get("x-forwarded-proto") ?? (host.startsWith("localhost") ? "http" : "https");
  const origin = `${protocol}://${host}`;
  const socialImage = new URL("/og.png", origin).toString();

  return {
    title: "星地波位规划台 · CN-G01",
    description: "中国区域 NTN 两级波位编排与多星接管规划界面。",
    icons: { icon: "/favicon.svg", shortcut: "/favicon.svg" },
    openGraph: {
      title: "星地波位规划台",
      description: "CN-G01 · 全国两级波位编排",
      images: [{ url: socialImage, width: 1536, height: 1024, alt: "全国 NTN 两级波位编排示意" }],
    },
    twitter: { card: "summary_large_image", title: "星地波位规划台", description: "CN-G01 · 全国两级波位编排", images: [socialImage] },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="zh-CN"><body>{children}</body></html>;
}
