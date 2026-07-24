import type { Metadata } from "next";
import { GlobalPlanner } from "./global-planner";

export const dynamic = "force-static";

export const metadata: Metadata = {
  title: "NTN全球陆地接入方案",
  description: "42×84、F=1星座的全球陆地覆盖检查、单星容量结论与星载双小区80 ms接入日历。",
};

export default function Home() {
  return <GlobalPlanner />;
}
