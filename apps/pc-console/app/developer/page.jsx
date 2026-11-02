"use client";

// /developer 子入口：复用「小悟众创」门户组件，与 / 共用同一登录态（同源 localStorage）。
// 首页 / 保持门户不变；此处把开发者区固定到 /developer，作为从首页 header「开发者中心」进入的子入口。
import DeveloperCenter from "../page";

export default function DeveloperPage() {
  return <DeveloperCenter />;
}
