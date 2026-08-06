export type NormalizedViewWindow = {
  start: number;
  end: number;
};

export const clampChartValue = (value: number, minimum: number, maximum: number) =>
  Math.min(maximum, Math.max(minimum, value));

export const normalizeChartView = (
  nextStart: number,
  nextEnd: number,
  minimumSpan = 0.01,
): NormalizedViewWindow => {
  const span = clampChartValue(nextEnd - nextStart, minimumSpan, 1);
  const start = clampChartValue(nextStart, 0, 1 - span);
  return { start, end: start + span };
};

export const zoomChartView = (
  view: NormalizedViewWindow,
  factor: number,
  anchor = 0.5,
  minimumSpan = 0.01,
): NormalizedViewWindow => {
  const currentSpan = view.end - view.start;
  const safeAnchor = clampChartValue(anchor, 0, 1);
  const nextSpan = clampChartValue(currentSpan * factor, minimumSpan, 1);
  const anchorPosition = view.start + currentSpan * safeAnchor;
  const nextStart = anchorPosition - nextSpan * safeAnchor;
  return normalizeChartView(nextStart, nextStart + nextSpan, minimumSpan);
};

export const panChartView = (
  view: NormalizedViewWindow,
  fractionOfCurrentSpan: number,
): NormalizedViewWindow => {
  const span = view.end - view.start;
  return normalizeChartView(
    view.start + span * fractionOfCurrentSpan,
    view.end + span * fractionOfCurrentSpan,
    Math.min(0.01, span),
  );
};
