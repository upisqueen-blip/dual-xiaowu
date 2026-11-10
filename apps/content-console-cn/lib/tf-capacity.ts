// 「本地 TF 卡」面板的容量解析与展示。
//
// 背景：设备侧 SdCardStorage::Status() 曾经用 snprintf("%llu") 拼容量文本，
// 而固件开了 CONFIG_LIBC_NEWLIB_NANO_FORMAT（精简版 printf，不支持 ll 长度
// 修饰符），于是 "total=%llu MB" 被原样吐成 "total=lu MB"，面板上就出现了
// 那句看不懂的 "total=lu MB, free=lu MB"。
//
// 设备端已改为 std::to_string 并单独上报 total_mb / free_mb 数字字段；
// 但已经在用户手里的旧固件还会把脏文本传上来，所以这里做两件事：
//   1. 优先用数字字段，其次从 status 文本里解析，兼容新旧固件；
//   2. 识别并丢弃 "total=lu" 这种 nano-printf 残留，绝不再把它显示给用户。

export type TfStorageSnapshot = {
  mounted?: boolean;
  status?: string | null;
  total_mb?: number | null;
  free_mb?: number | null;
};

export type TfCapacity = {
  totalMb: number | null;
  freeMb: number | null;
};

// nano printf 不支持 %llu 时留下的痕迹：total=lu / free=lu
const NANO_PRINTF_ARTIFACT = /\b(total|free)\s*=\s*lu\b/i;
// 正常的 "total=14900 MB" / "free=7.2 GB"
const MOUNTED_STATUS = /^TF\s+(card\s+)?mounted/i;

function toMb(value: number, unit?: string): number | null {
  if (!Number.isFinite(value) || value < 0) return null;
  switch ((unit ?? "MB").toUpperCase()) {
    case "TB":
      return value * 1024 * 1024;
    case "GB":
      return value * 1024;
    case "KB":
      return value / 1024;
    case "B":
      return value / (1024 * 1024);
    default:
      return value;
  }
}

function parseStatusSize(status: string, label: "total" | "free"): number | null {
  const matched = status.match(new RegExp(`${label}\\s*=\\s*(\\d+(?:\\.\\d+)?)\\s*([KMGT]?B)?`, "i"));
  if (!matched) return null;
  return toMb(Number(matched[1]), matched[2]);
}

/** 设备是否上报了 nano-printf 造成的脏文本（旧固件特征）。 */
export function isGarbledTfStatus(status?: string | null): boolean {
  return typeof status === "string" && NANO_PRINTF_ARTIFACT.test(status);
}

/**
 * 取容量：数字字段优先（新固件），其次解析 status 文本（过渡期固件），
 * 都拿不到就返回 null，由界面显示「待上报」而不是瞎猜。
 */
export function resolveTfCapacity(storage?: TfStorageSnapshot | null): TfCapacity {
  if (!storage) return { totalMb: null, freeMb: null };
  const status = typeof storage.status === "string" ? storage.status : "";
  const totalField = typeof storage.total_mb === "number" ? toMb(storage.total_mb) : null;
  const freeField = typeof storage.free_mb === "number" ? toMb(storage.free_mb) : null;
  return {
    totalMb: totalField ?? parseStatusSize(status, "total"),
    freeMb: freeField ?? parseStatusSize(status, "free"),
  };
}

/** 14900 → "14.6 GB"，900 → "900 MB"，拿不到 → fallback。 */
export function formatCapacity(mb?: number | null, fallback = "—"): string {
  if (mb == null || !Number.isFinite(mb) || mb < 0) return fallback;
  if (mb >= 1024 * 1024) return `${(mb / (1024 * 1024)).toFixed(1)} TB`;
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${Math.round(mb)} MB`;
}

/**
 * 面板里那行灰色小字：只在能提供额外信息时返回文本。
 * - 未挂载：原样返回原因（挂载失败的具体原因很有用）。
 * - 已挂载：Status() 的 "TF mounted; total=... free=..." 与上面的容量行重复，
 *   脏文本（total=lu）更不能显示，一律返回空字符串。
 */
export function tfStatusNote(storage?: TfStorageSnapshot | null): string {
  const status = typeof storage?.status === "string" ? storage.status.trim() : "";
  if (!status) return "";
  if (storage?.mounted === false) return status;
  if (isGarbledTfStatus(status)) return "";
  if (MOUNTED_STATUS.test(status)) return "";
  return status;
}
