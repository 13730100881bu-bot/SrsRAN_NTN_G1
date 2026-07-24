import type { NextConfig } from "next";

const githubPagesBuild = process.env.NTN_GITHUB_PAGES === "1";

const nextConfig: NextConfig = {
  output: githubPagesBuild ? "export" : undefined,
  trailingSlash: githubPagesBuild ? true : undefined,
};

export default nextConfig;
