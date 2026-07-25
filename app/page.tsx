import type { Metadata } from "next";
import { GlobalPlanner } from "./global-planner";

export const dynamic = "force-static";

export const metadata: Metadata = {
  title: "NTN全球陆地接入方案",
  description: "对比不同卫星规模，展示全球陆地覆盖、一级波位唯一分配，以及星载双小区接入日历。",
};

export default function Home() {
  return <GlobalPlanner />;
}
