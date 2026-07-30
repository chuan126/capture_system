"use client";

import { useState } from "react";

type PageId = "dashboard" | "tasks" | "playback" | "report";
type PlaybackTab = "result" | "history" | "logs";
type TaskTab = "pending" | "completed" | "abnormal";
type ViewMode = "3d" | "top" | "section";

const navigation: Array<{ id: PageId; label: string; index: string }> = [
  { id: "dashboard", label: "采集首页", index: "01" },
  { id: "tasks", label: "任务管理", index: "02" },
  { id: "playback", label: "数据回放", index: "03" },
  { id: "report", label: "报告导出", index: "04" },
];

function StatusPill({
  children,
  tone = "idle",
}: {
  children: React.ReactNode;
  tone?: "idle" | "ok" | "warn" | "danger";
}) {
  return (
    <span className={`status-pill status-pill--${tone}`}>
      <i />
      {children}
    </span>
  );
}

function PanelHead({
  title,
  description,
  trailing,
}: {
  title: string;
  description?: string;
  trailing?: React.ReactNode;
}) {
  return (
    <div className="panel-head">
      <div>
        <h2>{title}</h2>
        {description && <p>{description}</p>}
      </div>
      {trailing}
    </div>
  );
}

function EmptyChart({ historical = false }: { historical?: boolean }) {
  return (
    <div className="chart">
      <div className="chart__grid" />
      <div className="chart__axis chart__axis--y">
        <span>6.0</span><span>5.5</span><span>5.0</span><span>4.5</span>
      </div>
      <div className="chart__axis chart__axis--x">
        <span>0</span><span>25</span><span>50</span><span>75</span><span>100</span>
      </div>
      <EmptyState
        compact
        icon="⌁"
        title={historical ? "请选择任务查看净空曲线" : "等待净空测量数据"}
        description={historical ? "历史任务接入后显示" : "采集开始后在此显示实时结果"}
      />
    </div>
  );
}

function EmptyState({
  icon,
  title,
  description,
  compact = false,
}: {
  icon: string;
  title: string;
  description: string;
  compact?: boolean;
}) {
  return (
    <div className={`empty-state${compact ? " empty-state--compact" : ""}`}>
      <span className="empty-state__icon">{icon}</span>
      <strong>{title}</strong>
      <small>{description}</small>
    </div>
  );
}

function Header({ page }: { page: PageId }) {
  const title = navigation.find((item) => item.id === page)?.label;
  return (
    <header className="topbar">
      <div>
        <div className="breadcrumb">三维采集系统 / {title}</div>
        <h1>车载隧道净空高度测量</h1>
        <p>Odin1 Lite 激光雷达 · RK3588 设备端显控界面</p>
      </div>
      <div className="topbar__right">
        <div className="connection">
          <span>系统状态</span>
          <StatusPill>等待接入</StatusPill>
        </div>
        <div className="clock">
          <strong>--:--</strong>
          <span>设备时间</span>
        </div>
      </div>
    </header>
  );
}

function MetricCard({
  label,
  value,
  unit,
  hint,
  icon,
  primary = false,
}: {
  label: string;
  value: string;
  unit?: string;
  hint: string;
  icon: string;
  primary?: boolean;
}) {
  return (
    <article className={`metric-card${primary ? " metric-card--primary" : ""}`}>
      <div className="metric-card__top">
        <span>{label}</span>
        <i className="metric-icon">{icon}</i>
      </div>
      <strong className={value.length > 4 ? "metric-card__text-value" : ""}>
        {value}{unit && <small>{unit}</small>}
      </strong>
      <p>{hint}</p>
    </article>
  );
}

function Dashboard() {
  const [viewMode, setViewMode] = useState<ViewMode>("3d");
  const modes: Array<{ id: ViewMode; label: string }> = [
    { id: "3d", label: "三维视图" },
    { id: "top", label: "俯视图" },
    { id: "section", label: "断面视图" },
  ];

  return (
    <div className="page-stack">
      <section className="metric-grid">
        <MetricCard label="当前净空高度" value="--" unit="m" hint="等待有效测量结果" icon="H" primary />
        <MetricCard label="任务最低净空" value="--" unit="m" hint="任务开始后自动统计" icon="↓" />
        <MetricCard label="RTK 定位状态" value="未接入" hint="等待定位质量信息" icon="◎" />
        <MetricCard label="有效处理帧率" value="--" unit="FPS" hint="Odin1 Lite 点云输入" icon="≈" />
      </section>

      <section className="dashboard-layout">
        <div className="dashboard-main">
          <article className="panel cloud-panel dashboard-cloud-panel">
            <div className="cloud-toolbar">
              <div>
                <h2>点云空间预览</h2>
                <p>Odin1 Lite 降采样点云 · 预览链路与核心计算隔离</p>
              </div>
              <div className="segmented segmented--small" aria-label="点云视角">
                {modes.map((mode) => (
                  <button
                    type="button"
                    key={mode.id}
                    className={viewMode === mode.id ? "active" : ""}
                    onClick={() => setViewMode(mode.id)}
                  >
                    {mode.label}
                  </button>
                ))}
              </div>
              <span className="panel-tag panel-tag--muted">0 点</span>
            </div>
            <div className={`cloud-stage dashboard-cloud-stage cloud-stage--${viewMode}`}>
              <div className="cloud-stage__floor" />
              <div className="axis axis--x">X</div>
              <div className="axis axis--y">Y</div>
              <div className="axis axis--z">Z</div>
              <div className="orientation-cube">
                <span>TOP</span><i>FRONT</i><b>RIGHT</b>
              </div>
              <EmptyState
                icon="⁙"
                title="等待点云预览数据"
                description={`${modes.find((mode) => mode.id === viewMode)?.label}将在数据接入后显示`}
                compact
              />
              <div className="cloud-stage__footer">
                <span>车辆坐标系：X 向前 · Y 向左 · Z 向上</span>
                <span>当前帧：0 点 · 显示范围：-- m</span>
              </div>
            </div>
          </article>

          <article className="panel">
            <PanelHead
              title="实时净空高度曲线"
              description="当前任务的净空变化与高度阈值"
              trailing={
                <div className="legend">
                  <span><i />实时净空</span>
                  <span><i />高度阈值</span>
                </div>
              }
            />
            <EmptyChart />
          </article>
        </div>

        <div className="dashboard-side">
          <article className="panel current-task">
            <PanelHead
              title="当前检测任务"
              description="任务状态与切换信息"
              trailing={<span className="panel-tag panel-tag--muted">未开始</span>}
            />
            <div className="task-identity">
              <div><span>隧道编号</span><strong>--</strong></div>
              <div><span>当前任务</span><strong>尚未选择</strong></div>
              <div><span>下一任务</span><strong>暂无排队任务</strong></div>
              <div><span>作业车道</span><strong>--</strong></div>
            </div>
            <div className="progress-block">
              <div><span>任务进度</span><strong>0%</strong></div>
              <div className="progress-track"><i /></div>
            </div>
          </article>

          <article className="panel control-panel">
            <PanelHead title="采集任务管理" description="测量参数与采集控制" />
            <div className="form-grid">
              <label>
                <span>雷达安装高度 / m</span>
                <input placeholder="请输入安装高度" inputMode="decimal" />
              </label>
              <label>
                <span>净空高度阈值 / m</span>
                <input placeholder="请输入报警阈值" inputMode="decimal" />
              </label>
              <label className="form-grid__full">
                <span>测量参数配置</span>
                <select defaultValue="">
                  <option value="">请选择参数方案</option>
                </select>
              </label>
            </div>
            <div className="button-grid button-grid--control">
              <button type="button" className="button button--green">初始化并开始</button>
              <button type="button" className="button button--warning">暂停 / 恢复</button>
              <button type="button" className="button button--danger button-grid__full">停止采集并保存</button>
            </div>
            <label className="switch-row">
              <span>
                <strong>自动切换下一任务</strong>
                <small>当前任务完成后加载下一任务</small>
              </span>
              <input type="checkbox" aria-label="自动切换下一任务" />
              <i />
            </label>
          </article>

          <article className="panel status-panel">
            <PanelHead title="设备状态" description="现场设备与数据链路" />
            <div className="device-grid">
              <div><span><i>雷</i>Odin1 Lite</span><StatusPill>等待接入</StatusPill></div>
              <div><span><i>RTK</i>RTK 定位</span><StatusPill>等待接入</StatusPill></div>
              <div><span><i>控</i>主控状态</span><StatusPill>等待接入</StatusPill></div>
              <div><span><i>存</i>存储状态</span><strong>--</strong></div>
              <div><span><i>温</i>设备温度</span><strong>-- ℃</strong></div>
            </div>
          </article>
        </div>
      </section>

      <section className="alert-grid">
        <article className="panel">
          <PanelHead
            title="异常报警提示"
            description="净空阈值与设备异常集中显示"
            trailing={<StatusPill tone="ok">当前无报警</StatusPill>}
          />
          <div className="alert-empty">
            <span>✓</span>
            <div><strong>系统暂无报警信息</strong><small>出现高度阈值或设备异常时将在此提示</small></div>
          </div>
        </article>
        <article className="panel quick-status">
          <PanelHead title="测量状态" description="当前帧测量质量摘要" />
          <div>
            <span>测量质量<strong>暂无结果</strong></span>
            <span>有效点比例<strong>-- %</strong></span>
            <span>数据延迟<strong>-- ms</strong></span>
          </div>
        </article>
      </section>
    </div>
  );
}

function Tasks() {
  const [taskTab, setTaskTab] = useState<TaskTab>("pending");
  const tabs: Array<{ id: TaskTab; label: string }> = [
    { id: "pending", label: "待检测任务" },
    { id: "completed", label: "已完成任务" },
    { id: "abnormal", label: "异常任务" },
  ];

  return (
    <div className="page-stack">
      <section className="task-overview">
        <article><i>待</i><div><span>待检测任务</span><strong>--</strong></div></article>
        <article><i>完</i><div><span>已完成任务</span><strong>--</strong></div></article>
        <article><i>异</i><div><span>异常任务</span><strong>--</strong></div></article>
      </section>

      <section className="task-page-grid">
        <article className="panel task-create-panel">
          <PanelHead
            title="批量创建检测任务"
            description="按隧道与车道建立待检测任务"
            trailing={<span className="panel-tag">新建任务</span>}
          />
          <div className="task-form">
            <label>
              <span>隧道名称</span>
              <input placeholder="请输入隧道名称" />
            </label>
            <label>
              <span>隧道编号</span>
              <input placeholder="请输入隧道编号" />
            </label>
            <fieldset>
              <legend>车道选择</legend>
              <label className="check-card"><input type="checkbox" /><span><i>01</i><strong>左侧车道</strong></span></label>
              <label className="check-card"><input type="checkbox" /><span><i>02</i><strong>右侧车道</strong></span></label>
            </fieldset>
            <label>
              <span>任务编号规则</span>
              <input placeholder="例如：隧道编号-车道-序号" />
            </label>
            <label>
              <span>计划检测日期</span>
              <input type="date" />
            </label>
          </div>
          <div className="task-form-actions">
            <button type="button" className="button button--soft">清空内容</button>
            <button type="button" className="button button--primary">批量创建任务</button>
          </div>
          <div className="info-box">当前为静态界面，任务内容不会保存或提交。</div>
        </article>

        <article className="panel task-queue-panel">
          <PanelHead title="任务编排预览" description="确认任务顺序与自动切换关系" />
          <div className="queue-empty">
            <EmptyState icon="≡" title="尚未添加任务" description="填写左侧信息后在此预览任务顺序" />
          </div>
          <div className="queue-rule">
            <div><span>自动切换规则</span><StatusPill>未设置</StatusPill></div>
            <p>当前任务完成 → 加载下一任务 → 更新任务状态</p>
          </div>
        </article>
      </section>

      <article className="panel table-panel">
        <div className="table-toolbar">
          <div className="segmented">
            {tabs.map((tab) => (
              <button
                type="button"
                key={tab.id}
                className={taskTab === tab.id ? "active" : ""}
                onClick={() => setTaskTab(tab.id)}
              >
                {tab.label}<span>0</span>
              </button>
            ))}
          </div>
          <div className="toolbar-actions">
            <label className="search-box"><span>⌕</span><input placeholder="搜索任务编号或隧道名称" /></label>
            <select defaultValue=""><option value="">全部车道</option></select>
          </div>
        </div>
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>任务编号</th><th>隧道名称</th><th>隧道编号</th>
                <th>检测车道</th><th>计划日期</th><th>任务状态</th><th>操作</th>
              </tr>
            </thead>
            <tbody>
              <tr className="empty-row">
                <td colSpan={7}>暂无{tabs.find((tab) => tab.id === taskTab)?.label}</td>
              </tr>
            </tbody>
          </table>
        </div>
      </article>
    </div>
  );
}

function Playback() {
  const [tab, setTab] = useState<PlaybackTab>("result");
  const tabs: Array<{ id: PlaybackTab; label: string }> = [
    { id: "result", label: "测量结果" },
    { id: "history", label: "历史数据" },
    { id: "logs", label: "运行日志" },
  ];

  return (
    <div className="page-stack">
      <article className="panel playback-filter">
        <div>
          <span>历史数据回放</span>
          <strong>选择检测任务，查看测量结果与历史记录</strong>
        </div>
        <label><span>检测任务</span><select defaultValue=""><option value="">请选择任务</option></select></label>
        <label><span>回放区间</span><select defaultValue="all"><option value="all">全部数据</option></select></label>
        <button type="button" className="button button--primary">载入任务</button>
      </article>

      <div className="section-tabs">
        {tabs.map((item) => (
          <button
            type="button"
            key={item.id}
            className={tab === item.id ? "active" : ""}
            onClick={() => setTab(item.id)}
          >
            {item.label}
          </button>
        ))}
      </div>

      {tab === "result" && (
        <>
          <section className="playback-summary">
            <article><span>最低净空</span><strong>-- <small>m</small></strong></article>
            <article><span>有效测量点</span><strong>-- <small>点</small></strong></article>
            <article><span>采集时长</span><strong>--:--</strong></article>
            <article><span>异常点数量</span><strong>-- <small>点</small></strong></article>
          </section>
          <article className="panel">
            <PanelHead
              title="净空变化曲线"
              description="按采集序号查看历史净空结果"
              trailing={<span className="panel-tag panel-tag--muted">暂无任务</span>}
            />
            <EmptyChart historical />
          </article>
          <MeasurementTable message="请选择检测任务查看测量结果" />
        </>
      )}

      {tab === "history" && (
        <article className="panel data-management">
          <PanelHead title="历史测量数据" description="测量数据保存与历史任务管理" />
          <div className="data-actions">
            <button type="button"><i>数</i><span><strong>测量数据保存</strong><small>查看结构化测量结果</small></span><b>›</b></button>
            <button type="button"><i>史</i><span><strong>历史测量数据</strong><small>按任务查询历史记录</small></span><b>›</b></button>
            <button type="button"><i>任</i><span><strong>历史任务管理</strong><small>查看已完成与异常任务</small></span><b>›</b></button>
          </div>
          <div className="data-empty"><EmptyState icon="▤" title="尚未选择历史任务" description="载入任务后显示数据文件与记录" /></div>
        </article>
      )}

      {tab === "logs" && (
        <article className="panel log-panel">
          <PanelHead
            title="运行日志"
            description="查看任务采集与设备运行记录"
            trailing={<button type="button" className="button button--soft">导出日志</button>}
          />
          <div className="log-toolbar">
            <select defaultValue="all"><option value="all">全部级别</option><option value="info">信息</option><option value="warn">警告</option></select>
            <input placeholder="搜索日志内容" />
          </div>
          <div className="log-empty"><EmptyState icon="≡" title="暂无日志记录" description="选择任务后显示对应运行日志" /></div>
        </article>
      )}
    </div>
  );
}

function MeasurementTable({ message }: { message: string }) {
  return (
    <article className="panel table-panel">
      <PanelHead title="测量数据明细" description="净空高度、最低点位置和质量状态" />
      <div className="table-wrap">
        <table>
          <thead>
            <tr>
              <th>序号</th><th>采集时间</th><th>相对里程</th><th>净空高度</th>
              <th>最低点横向位置</th><th>测量质量</th><th>标记</th>
            </tr>
          </thead>
          <tbody><tr className="empty-row"><td colSpan={7}>{message}</td></tr></tbody>
        </table>
      </div>
    </article>
  );
}

function Report() {
  return (
    <div className="page-stack">
      <article className="panel report-filter">
        <div>
          <span>检测结果输出</span>
          <strong>选择已完成任务并设置导出内容</strong>
        </div>
        <label><span>检测任务</span><select defaultValue=""><option value="">请选择已完成任务</option></select></label>
        <button type="button" className="button button--primary">载入报告信息</button>
      </article>

      <section className="report-layout">
        <div className="report-main">
          <article className="panel">
            <PanelHead title="报告导出" description="选择文件类型与报告内容" />
            <div className="export-list">
              <label className="export-option">
                <input type="radio" name="exportType" defaultChecked />
                <div className="file-icon">CSV</div>
                <span><strong>测量数据表</strong><small>逐测量点净空、位置与质量状态</small></span>
                <i>数据表格</i>
              </label>
              <label className="export-option">
                <input type="radio" name="exportType" />
                <div className="file-icon file-icon--blue">PDF</div>
                <span><strong>综合检测报告</strong><small>任务摘要、最低净空与诊断信息</small></span>
                <i>正式报告</i>
              </label>
            </div>
            <fieldset className="report-content-options">
              <legend>报告内容</legend>
              <label><input type="checkbox" defaultChecked />任务基本信息</label>
              <label><input type="checkbox" defaultChecked />进出隧道 RTK</label>
              <label><input type="checkbox" defaultChecked />最低净空结果</label>
              <label><input type="checkbox" defaultChecked />净空变化曲线</label>
              <label><input type="checkbox" defaultChecked />数据质量摘要</label>
              <label><input type="checkbox" />运行日志摘要</label>
            </fieldset>
            <div className="export-actions">
              <button type="button" className="button button--soft">预览报告</button>
              <button type="button" className="button button--primary">生成并导出</button>
            </div>
          </article>

          <article className="panel report-preview">
            <PanelHead
              title="报告预览"
              description="加载任务后显示报告页面"
              trailing={<span className="panel-tag panel-tag--muted">未生成</span>}
            />
            <div className="paper">
              <div className="paper__header"><i>T</i><span>隧道净空综合检测报告</span></div>
              <EmptyState icon="▧" title="暂无报告预览" description="选择任务并点击预览报告" />
              <div className="paper__footer">三维采集系统 · 隧道净空测量显控终端</div>
            </div>
          </article>
        </div>

        <aside className="report-side">
          <article className="panel">
            <PanelHead
              title="任务摘要"
              description="随所选任务自动更新"
              trailing={<span className="panel-tag panel-tag--muted">未选择</span>}
            />
            <div className="summary">
              <div><span>隧道名称</span><strong>--</strong></div>
              <div><span>任务编号</span><strong>--</strong></div>
              <div><span>检测车道</span><strong>--</strong></div>
              <div><span>采集时长</span><strong>--:--</strong></div>
              <div><span>最低净空</span><strong>-- m</strong></div>
              <div><span>入口 RTK</span><strong>待选择任务</strong></div>
              <div><span>出口 RTK</span><strong>待选择任务</strong></div>
              <div><span>有效测量点</span><strong>--</strong></div>
              <div><span>有效帧数</span><strong>--</strong></div>
            </div>
          </article>
          <article className="panel file-status">
            <PanelHead title="导出状态" description="报告文件生成情况" />
            <div><i>↧</i><span><strong>尚未生成文件</strong><small>完成报告设置后开始导出</small></span></div>
          </article>
        </aside>
      </section>
    </div>
  );
}

export default function Home() {
  const [activePage, setActivePage] = useState<PageId>("dashboard");

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand__mark"><span>T</span></div>
          <div><strong>三维采集系统</strong><span>隧道净空测量终端</span></div>
        </div>
        <div className="nav-label">工作台</div>
        <nav aria-label="主导航">
          {navigation.map((item) => (
            <button
              type="button"
              key={item.id}
              className={activePage === item.id ? "active" : ""}
              onClick={() => setActivePage(item.id)}
              aria-current={activePage === item.id ? "page" : undefined}
            >
              <span>{item.index}</span>{item.label}<i>›</i>
            </button>
          ))}
        </nav>
        <div className="sidebar__bottom">
          <div className="device-card">
            <div className="device-card__icon">RK</div>
            <div><strong>车载主控终端</strong><span>RK3588 · 本地运行</span></div>
            <i />
          </div>
          <div className="version">CAPTURE SYSTEM · V1.0</div>
        </div>
      </aside>
      <main>
        <Header page={activePage} />
        <div className="page-content">
          {activePage === "dashboard" && <Dashboard />}
          {activePage === "tasks" && <Tasks />}
          {activePage === "playback" && <Playback />}
          {activePage === "report" && <Report />}
        </div>
      </main>
    </div>
  );
}
