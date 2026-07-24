const publicBasePath =
  import.meta.env.VITE_NTN_PUBLIC_BASE_PATH?.replace(/\/$/, "") ?? "";

export function publicPath(path: string): string {
  const normalizedPath = path.startsWith("/") ? path : `/${path}`;
  return `${publicBasePath}${normalizedPath}`;
}
