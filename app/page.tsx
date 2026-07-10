import type { Metadata } from "next";
import { BeamPlanner } from "./beam-planner";

export const metadata: Metadata = {
  title: "星地波位规划台 · CN-G01",
  description: "面向中国区域的 NTN 两级波位编排、复用规划与多星接管可视化。",
};

export default function Home() {
  return <BeamPlanner />;
}
