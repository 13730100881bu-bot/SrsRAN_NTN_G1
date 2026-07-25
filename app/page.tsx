import type { Metadata } from "next";
import { GlobalPlanner } from "./global-planner";

export const dynamic = "force-static";

export const metadata: Metadata = {
  title: "NTN全球陆地接入方案",
  description: "最终方案采用2,990颗卫星，展示全球陆地覆盖、一级波位唯一分配和星载双小区接入日历。",
};

export default function Home() {
  return <GlobalPlanner />;
}
