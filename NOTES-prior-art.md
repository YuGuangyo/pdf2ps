# 现成 PDF→PostScript 方案调研结论（2026-10-19）

目标环境：京东云一代（ramips/mt7621，**mipsel + musl 1.2 + 软浮点**），
`/overlay` 可用空间 **约 11.5 MB**，CUPS 2.3.3-r1 缺 PDF 转换链。

## 逐一排除

| 方案 | 结论 | 关键数据 |
|---|---|---|
| OpenWrt / ImmortalWrt 官方源 | **不存在** | 9200+ 包里没有 ghostscript / poppler / pdftops / mupdf |
| Alpine mipsel | **不存在** | Alpine 自 v3.16 起不再构建 mipsel |
| Debian bookworm mipsel poppler-utils | **装不下** | 递归依赖闭包 **51.5 MB**，其中 libc6 单项 23.3 MB |
| Debian sid mipsel | **已移除** | sid 仅剩 armhf / armv8l / ppc64el / s390x / x86 / x86_64 等 |
| [Vladdrako/openwrt-printing-packages](https://github.com/Vladdrako/openwrt-printing-packages) | **不适用** | 需自行交叉编译；README 明说"这些包需要 100 MB 以上空间" |
| [rdrkr/openwrt-printing](https://github.com/rdrkr/openwrt-printing) | **不适用** | aarch64 FLINT3 专用；但其中 pdftops shim 思路可借鉴 |
| xpdf / MuPDF | **体积超标** | 都是完整 PDF 引擎 + 字体库，远超 11.5 MB |

## 唯一可行路径

自研极简 PDF→PostScript 转换器（pdftops-lite），针对本场景裁剪：

- 只输出 PostScript，**不做光栅化**，所以不需要 FreeType/字体渲染库
- 只用 Standard-14 基础字体 + Type3 字形路径 + 嵌入 TrueType glyf 取轮廓
- 静态链接 musl，产物约 200 KB

## 从 rdrkr/openwrt-printing 学到的印证

该项目在 aarch64 上也遇到并记录了我们此前的诊断：

> 第一个症状是 `application/pdf` 和 `image/urf` 不在 `ipptool … document-format-supported`
> 列表里；因为 **iOS AirPrint 提交任务前会先探测这个列表**，所以打印机显示"在线"，
> 但 **Create-Job 根本不会到达 CUPS**。

这与我们实测的 iOS/AirPrint 行为完全一致，说明此前对
`/etc/cups/mime.convs` 补 PDF 规则的修法方向正确。
