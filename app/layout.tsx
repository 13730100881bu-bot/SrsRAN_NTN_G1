import type { Metadata } from "next";
import "./globals.css";
import "./global.css";
import { publicPath } from "./public-path";

export async function generateMetadata(): Promise<Metadata> {
  return {
    title: "NTN全球陆地接入方案",
    description: "最终方案采用2,990颗卫星，展示全球陆地覆盖、一级波位唯一分配和星载双小区接入日历。",
    icons: { icon: publicPath("/favicon.svg"), shortcut: publicPath("/favicon.svg") },
    openGraph: {
      title: "NTN全球陆地接入方案",
      description: "最终方案采用2,990颗卫星 · 可见清单与实际服务负载分开计算 · 上线前验收继续进行",
    },
    twitter: { card: "summary", title: "NTN全球陆地接入方案", description: "全球陆地覆盖、唯一服务分配与星载跳波束日历" },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="zh-CN"><body>{children}</body></html>;
}
