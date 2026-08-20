"use client";

import {
  useEffect,
  useMemo,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
  type PointerEvent as ReactPointerEvent,
} from "react";

import {
  clampChartValue,
  fitChartYRange,
  normalizeChartView,
  panChartView,
  panChartYRange,
  zoomChartYRange,
  zoomChartView,
  type ChartYRange,
  type NormalizedViewWindow,
} from "@/components/playback/clearanceChartViewport";
import { markPlaybackTiming } from "@/components/playback/playbackPerformance";

export type ClearanceSample = {
  sampleIndex: number;
  timestampMs: number;
  elapsedMs: number;
  heightM: number | null;
  valid: boolean;
  reason?: string;
};

type InteractiveClearanceChartProps = {
  samples: ClearanceSample[];
  domainStartTimestampMs: number;
  domainEndTimestampMs: number;
  initialView?: NormalizedViewWindow;
  onViewWindowChange?: (view: NormalizedViewWindow) => void;
  timingTaskId?: string;
  emptyTitle: string;
  emptyDescription: string;
};

type DragState = {
  pointerId: number;
  startX: number;
  startY: number;
  view: NormalizedViewWindow;
  yRange: ChartYRange;
};

type HoverPoint = {
  x: number;
  y: number;
  sample: ClearanceSample;
};

const PLOT = {
  left: 72,
  right: 28,
  top: 30,
  bottom: 52,
};

const formatTimestamp = (timestampMs: number) => {
  const date = new Date(timestampMs);
  return date.toLocaleTimeString("zh-CN", {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    fractionalSecondDigits: 3,
  });
};

const formatAxisTime = (timestampMs: number) => {
  const date = new Date(timestampMs);
  return date.toLocaleTimeString("zh-CN", {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
};

export default function InteractiveClearanceChart({
  samples,
  domainStartTimestampMs,
  domainEndTimestampMs,
  initialView,
  onViewWindowChange,
  timingTaskId,
  emptyTitle,
  emptyDescription,
}: InteractiveClearanceChartProps) {
  const chartRef = useRef<HTMLDivElement>(null);
  const dragRef = useRef<DragState | null>(null);
  const dragFrameRef = useRef<number | null>(null);
  const pendingDragViewRef = useRef<NormalizedViewWindow | null>(null);
  const pendingDragYRangeRef = useRef<ChartYRange | null>(null);
  const reportedInitialViewRef = useRef(false);
  const reportedSamplesRef = useRef(false);
  const reportedPathRef = useRef(false);
  const reportedVisibleRef = useRef(false);
  const [view, setView] = useState<NormalizedViewWindow>(initialView ?? { start: 0, end: 1 });
  const [yRange, setYRange] = useState<ChartYRange>(() => (
    fitChartYRange(samples.flatMap((sample) => (
      sample.valid && sample.heightM !== null ? [sample.heightM] : []
    ))) ?? { minimum: 0, maximum: 1 }
  ));
  const initialYRangeRef = useRef(yRange);
  const [hover, setHover] = useState<HoverPoint | null>(null);
  const [dragging, setDragging] = useState(false);

  const orderedSamples = useMemo(
    () => [...samples].sort((left, right) => left.timestampMs - right.timestampMs),
    [samples],
  );

  const fullStart = domainStartTimestampMs;
  const fullEnd = Math.max(domainStartTimestampMs + 1, domainEndTimestampMs);
  const fullDuration = Math.max(1, fullEnd - fullStart);
  const windowStart = fullStart + fullDuration * view.start;
  const windowEnd = fullStart + fullDuration * view.end;
  const hasData = orderedSamples.length > 1 && domainEndTimestampMs > domainStartTimestampMs;

  const visibleSamples = useMemo(() => {
    if (!hasData) return [];
    const margin = (windowEnd - windowStart) * 0.01;
    return orderedSamples.filter(
      (sample) => sample.timestampMs >= windowStart - margin && sample.timestampMs <= windowEnd + margin,
    );
  }, [hasData, orderedSamples, windowEnd, windowStart]);

  const yMin = yRange.minimum;
  const yMax = yRange.maximum;

  const outOfRange = useMemo(() => {
    let above = 0;
    let below = 0;
    visibleSamples.forEach((sample) => {
      if (!sample.valid || sample.heightM === null || !Number.isFinite(sample.heightM)) return;
      if (sample.heightM > yMax) above += 1;
      if (sample.heightM < yMin) below += 1;
    });
    return { above, below };
  }, [visibleSamples, yMax, yMin]);

  const xToPercent = (timestampMs: number) =>
    ((timestampMs - windowStart) / Math.max(1, windowEnd - windowStart)) * 100;
  const yToPercent = (heightM: number) =>
    100 - ((heightM - yMin) / Math.max(0.001, yMax - yMin)) * 100;

  const lineSegments = useMemo(() => {
    const nextSegments: string[][] = [];
    let currentSegment: string[] = [];

    visibleSamples.forEach((sample) => {
      if (!sample.valid || sample.heightM === null || !Number.isFinite(sample.heightM)) {
        if (currentSegment.length > 1) nextSegments.push(currentSegment);
        currentSegment = [];
        return;
      }
      const x = ((sample.timestampMs - windowStart) / Math.max(1, windowEnd - windowStart)) * 100;
      const y = 100 - ((sample.heightM - yMin) / Math.max(0.001, yMax - yMin)) * 100;
      currentSegment.push(`${x},${y}`);
    });

    if (currentSegment.length > 1) nextSegments.push(currentSegment);
    return nextSegments;
  }, [visibleSamples, windowEnd, windowStart, yMax, yMin]);

  const yTicks = useMemo(
    () => Array.from({ length: 5 }, (_, index) => yMax - ((yMax - yMin) * index) / 4),
    [yMax, yMin],
  );
  const xTicks = useMemo(
    () => Array.from({ length: 6 }, (_, index) => windowStart + ((windowEnd - windowStart) * index) / 5),
    [windowEnd, windowStart],
  );

  const zoomAt = (factor: number, anchor = 0.5) => {
    if (!hasData) return;
    setView((currentView) => zoomChartView(currentView, factor, anchor));
  };

  const panBy = (fraction: number) => {
    if (!hasData) return;
    setView((currentView) => panChartView(currentView, fraction));
  };

  const zoomVertically = (factor: number) => {
    if (!hasData) return;
    setYRange((current) => zoomChartYRange(current, factor));
  };

  const commitViewIfChanged = (nextView: NormalizedViewWindow) => {
    setView((current) => (
      Math.abs(current.start - nextView.start) < 1e-9
      && Math.abs(current.end - nextView.end) < 1e-9
        ? current
        : nextView
    ));
  };

  const resetView = () => {
    setView(initialView ?? { start: 0, end: 1 });
    setYRange(initialYRangeRef.current);
    setHover(null);
  };

  const plotMetrics = () => {
    const rect = chartRef.current?.getBoundingClientRect();
    if (!rect) return null;
    const width = Math.max(1, rect.width - PLOT.left - PLOT.right);
    const height = Math.max(1, rect.height - PLOT.top - PLOT.bottom);
    return { rect, width, height };
  };

  const pointerToPlotRatio = (clientX: number) => {
    const metrics = plotMetrics();
    if (!metrics) return 0.5;
    return clampChartValue((clientX - metrics.rect.left - PLOT.left) / metrics.width, 0, 1);
  };

  const updateHover = (clientX: number) => {
    if (!hasData || dragging || visibleSamples.length === 0) return;
    const metrics = plotMetrics();
    if (!metrics) return;

    const ratio = pointerToPlotRatio(clientX);
    const targetTime = windowStart + (windowEnd - windowStart) * ratio;
    let nearest = visibleSamples[0];
    let nearestDistance = Math.abs(nearest.timestampMs - targetTime);

    visibleSamples.forEach((sample) => {
      const distance = Math.abs(sample.timestampMs - targetTime);
      if (distance < nearestDistance) {
        nearest = sample;
        nearestDistance = distance;
      }
    });

    if (!nearest.valid || nearest.heightM === null) {
      setHover(null);
      return;
    }

    setHover({
      x: PLOT.left + (xToPercent(nearest.timestampMs) / 100) * metrics.width,
      y: PLOT.top + (yToPercent(nearest.heightM) / 100) * metrics.height,
      sample: nearest,
    });
  };

  useEffect(() => {
    const chart = chartRef.current;
    if (!chart) return undefined;

    const handleWheel = (event: WheelEvent) => {
      if (!hasData) return;
      event.preventDefault();
      event.stopPropagation();
      if (event.shiftKey) {
        setYRange((current) =>
          zoomChartYRange(current, event.deltaY > 0 ? 1 / 1.18 : 1.18),
        );
        return;
      }
      const rect = chart.getBoundingClientRect();
      const plotWidth = Math.max(1, rect.width - PLOT.left - PLOT.right);
      const anchor = clampChartValue(
        (event.clientX - rect.left - PLOT.left) / plotWidth,
        0,
        1,
      );
      setView((currentView) =>
        zoomChartView(currentView, event.deltaY > 0 ? 1.18 : 0.84, anchor),
      );
    };

    chart.addEventListener("wheel", handleWheel, { passive: false });
    return () => chart.removeEventListener("wheel", handleWheel);
  }, [hasData]);

  useEffect(() => () => {
    if (dragFrameRef.current !== null) window.cancelAnimationFrame(dragFrameRef.current);
  }, []);

  useEffect(() => {
    if (!reportedInitialViewRef.current) {
      reportedInitialViewRef.current = true;
      return;
    }
    onViewWindowChange?.(view);
  }, [onViewWindowChange, view]);

  useEffect(() => {
    if (!hasData || !timingTaskId) return undefined;
    if (!reportedSamplesRef.current) {
      reportedSamplesRef.current = true;
      markPlaybackTiming(timingTaskId, "chart component received samples", {
        receivedSamples: samples.length,
      });
    }
    if (!reportedPathRef.current) {
      reportedPathRef.current = true;
      markPlaybackTiming(timingTaskId, "chart path calculated", {
        pathSegments: lineSegments.length,
      });
    }
    if (reportedVisibleRef.current) return undefined;
    const frame = window.requestAnimationFrame(() => {
      reportedVisibleRef.current = true;
      markPlaybackTiming(timingTaskId, "chart visible", {
        receivedSamples: samples.length,
        visibleSamples: visibleSamples.length,
        pathSegments: lineSegments.length,
      });
    });
    return () => window.cancelAnimationFrame(frame);
  }, [hasData, lineSegments.length, samples.length, timingTaskId, visibleSamples.length]);

  const handlePointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (!hasData || event.button !== 0) return;
    event.currentTarget.setPointerCapture(event.pointerId);
    dragRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      view,
      yRange,
    };
    setDragging(true);
    setHover(null);
  };

  const handlePointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) {
      updateHover(event.clientX);
      return;
    }

    const metrics = plotMetrics();
    if (!metrics) return;
    const span = drag.view.end - drag.view.start;
    const delta = ((event.clientX - drag.startX) / metrics.width) * span;
    pendingDragViewRef.current = normalizeChartView(
      drag.view.start - delta,
      drag.view.end - delta,
    );
    pendingDragYRangeRef.current = panChartYRange(
      drag.yRange,
      (event.clientY - drag.startY) / metrics.height,
    );
    if (dragFrameRef.current === null) {
      dragFrameRef.current = window.requestAnimationFrame(() => {
        dragFrameRef.current = null;
        const nextView = pendingDragViewRef.current;
        const nextYRange = pendingDragYRangeRef.current;
        pendingDragViewRef.current = null;
        pendingDragYRangeRef.current = null;
        if (nextView) commitViewIfChanged(nextView);
        if (nextYRange) setYRange(nextYRange);
      });
    }
  };

  const finishPointer = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (dragRef.current?.pointerId === event.pointerId) {
      if (dragFrameRef.current !== null) {
        window.cancelAnimationFrame(dragFrameRef.current);
        dragFrameRef.current = null;
      }
      const finalView = pendingDragViewRef.current;
      const finalYRange = pendingDragYRangeRef.current;
      pendingDragViewRef.current = null;
      pendingDragYRangeRef.current = null;
      if (finalView) commitViewIfChanged(finalView);
      if (finalYRange) setYRange(finalYRange);
      dragRef.current = null;
      setDragging(false);
    }
  };

  const handleKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    if (!hasData) return;
    if (event.key === "+" || event.key === "=") zoomAt(0.8);
    if (event.key === "-") zoomAt(1.25);
    if (event.key === "ArrowLeft") panBy(-0.12);
    if (event.key === "ArrowRight") panBy(0.12);
    if (event.key === "ArrowUp") zoomVertically(1.2);
    if (event.key === "ArrowDown") zoomVertically(1 / 1.2);
    if (event.key === "0" || event.key === "Home") resetView();
  };

  const viewPercent = Math.max(0.01, (view.end - view.start) * 100);
  return (
    <div className="interactive-clearance-chart-shell">
      <div className="interactive-clearance-chart-toolbar" aria-label="曲线视图工具">
        <div>
          <strong>当前时间窗口</strong>
          <span>{hasData ? `任务范围 ${viewPercent.toFixed(viewPercent < 1 ? 2 : 0)}% · 纵轴已锁定` : "当前没有可显示记录"}</span>
        </div>
        <div className="interactive-clearance-chart-toolbar__guide">
          <span>拖拽：左右、上下平移</span>
          <span>滚轮：横向缩放</span>
          <span>Shift+滚轮：纵向缩放</span>
          <span>双击复位</span>
        </div>
        <div className="interactive-clearance-chart-toolbar__actions">
          <button type="button" disabled={!hasData} onClick={() => zoomAt(0.8)} aria-label="放大曲线">＋</button>
          <button type="button" disabled={!hasData} onClick={() => zoomAt(1.25)} aria-label="缩小曲线">－</button>
          <button type="button" disabled={!hasData || yMax - yMin <= 0.020001} onClick={() => zoomVertically(1.4)} aria-label="纵向放大曲线">Y＋</button>
          <button type="button" disabled={!hasData || yMax - yMin >= 39.999} onClick={() => zoomVertically(1 / 1.4)} aria-label="纵向缩小曲线">Y－</button>
          <button type="button" disabled={!hasData} onClick={resetView}>回到开头</button>
        </div>
      </div>

      <div
        ref={chartRef}
        className={`interactive-clearance-chart${dragging ? " is-dragging" : ""}${hasData ? " has-data" : " is-empty"}`}
        role="application"
        tabIndex={0}
        aria-label="可左右和上下拖拽、可缩放的任务净空高度曲线"
        onPointerDown={handlePointerDown}
        onPointerMove={handlePointerMove}
        onPointerUp={finishPointer}
        onPointerCancel={finishPointer}
        onPointerLeave={(event) => {
          finishPointer(event);
          setHover(null);
        }}
        onDoubleClick={resetView}
        onKeyDown={handleKeyDown}
      >
        <svg className="interactive-clearance-chart__svg" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
          <defs>
            <clipPath id="clearance-chart-clip">
              <rect x="0" y="0" width="100" height="100" />
            </clipPath>
          </defs>

          <g className="interactive-clearance-chart__grid">
            {yTicks.map((tick, index) => {
              const y = (100 * index) / 4;
              return <line key={`y-${tick}`} x1="0" x2="100" y1={y} y2={y} />;
            })}
            {xTicks.map((tick, index) => {
              const x = (100 * index) / 5;
              return <line key={`x-${tick}`} x1={x} x2={x} y1="0" y2="100" />;
            })}
          </g>

          <g className="interactive-clearance-chart__axes">
            <line x1="0" x2="0" y1="0" y2="100" />
            <line x1="0" x2="100" y1="100" y2="100" />
          </g>

          {hasData && (
            <g clipPath="url(#clearance-chart-clip)" className="interactive-clearance-chart__series">
              {lineSegments.map((points, index) => (
                <polyline key={index} points={points.join(" ")} />
              ))}
            </g>
          )}
        </svg>

        <div className="interactive-clearance-chart__y-axis" aria-hidden="true">
          <strong>净空高度 m</strong>
          <div>
            {yTicks.map((tick) => <span key={tick}>{hasData ? tick.toFixed(2) : "--"}</span>)}
          </div>
        </div>
        <div className="interactive-clearance-chart__x-axis" aria-hidden="true">
          {xTicks.map((tick) => <span key={tick}>{hasData ? formatAxisTime(tick) : "--:--:--"}</span>)}
        </div>

        {outOfRange.above > 0 && (
          <div className="interactive-clearance-chart__range-warning is-above">
            ↑ {outOfRange.above} 个点超出纵轴，可上下拖动或使用 Y－扩大范围
          </div>
        )}
        {outOfRange.below > 0 && (
          <div className="interactive-clearance-chart__range-warning is-below">
            ↓ {outOfRange.below} 个点超出纵轴，可上下拖动或使用 Y－扩大范围
          </div>
        )}

        {hover && (
          <div className="interactive-clearance-chart__hover" style={{ left: hover.x, top: hover.y }}>
            <i />
            <div>
              <strong>{hover.sample.heightM?.toFixed(3)} m</strong>
              <span>{formatTimestamp(hover.sample.timestampMs)}</span>
            </div>
          </div>
        )}

        {!hasData && (
          <div className="workflow-empty-state workflow-empty-state--compact">
            <strong>{emptyTitle}</strong>
            <p>{emptyDescription}</p>
          </div>
        )}
      </div>
    </div>
  );
}
