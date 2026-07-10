# NTN 星地波位规划台

中国区域两级波位编排的交互式 Web 界面，覆盖：

- 全国连续地固网格与 service / guard mask；
- 一级信令波位和七个二级数字业务波位；
- 七色复用、Tracking Area 与稳定波位 ID；
- 多颗卫星接管同一地固波位的四阶段过程。

当前规划值用于方案推演，不是协议常量。正式全国目录仍需接入运营范围、岛礁、海岸和边境保护带数据。

## 本地运行

Windows 下可直接双击 `open_ntn_beam_planner.cmd`。它会启动本机服务并打开：

`http://127.0.0.1:4317`

使用结束后关闭标题为 `NTN Beam Planner Server` 的命令行窗口即可。

也可以手动运行：

```bash
npm install
npm run dev -- -H 127.0.0.1 -p 4317
```

生产构建与渲染测试：

```bash
npm run build
npm test
```
