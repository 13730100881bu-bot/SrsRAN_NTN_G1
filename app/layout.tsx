import type { Metadata } from "next";
import "./globals.css";
import "./global.css";

export async function generateMetadata(): Promise<Metadata> {
  return {
    title: "NTN全球陆地接入方案",
    description: "42×84、F=1星座的全球陆地接入可行性结论、卫星负载与80 ms跳波束日历。",
    icons: { icon: "/favicon.svg", shortcut: "/favicon.svg" },
    openGraph: {
      title: "NTN全球陆地接入方案",
      description: "F=1工程候选 · 720/720离散覆盖检查 · 单星峰值209/256",
    },
    twitter: { card: "summary", title: "NTN全球陆地接入方案", description: "F=1工程候选、覆盖结论与星载跳波束日历" },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="zh-CN"><body>{children}</body></html>;
}
