import type { Metadata } from "next";
import "./globals.css";
import "./global.css";
import { publicPath } from "./public-path";

export async function generateMetadata(): Promise<Metadata> {
  return {
    title: "NTN全球陆地接入方案",
    description: "42轨道面、每面84星的全球陆地接入方案、卫星负载与80 ms跳波束日历。",
    icons: { icon: publicPath("/favicon.svg"), shortcut: publicPath("/favicon.svg") },
    openGraph: {
      title: "NTN全球陆地接入方案",
      description: "当前工程候选 · 720/720个检查时刻均有候选卫星 · 单星峰值209/256",
    },
    twitter: { card: "summary", title: "NTN全球陆地接入方案", description: "全球陆地覆盖方案、卫星负载与星载跳波束日历" },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="zh-CN"><body>{children}</body></html>;
}
