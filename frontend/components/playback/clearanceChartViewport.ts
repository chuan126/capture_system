export type NormalizedViewWindow = {
  start: number;
  end: number;
};

export type ChartYRange = {
  minimum: number;
  maximum: number;
};

export const clampChartValue = (value: number, minimum: number, maximum: number) =>
  Math.min(maximum, Math.max(minimum, value));

export const normalizeChartView = (
  nextStart: number,
  nextEnd: number,
  minimumSpan = 0.0001,
): NormalizedViewWindow => {
  const span = clampChartValue(nextEnd - nextStart, minimumSpan, 1);
  const start = clampChartValue(nextStart, 0, 1 - span);
  return { start, end: start + span };
};

export const zoomChartView = (
  view: NormalizedViewWindow,
  factor: number,
  anchor = 0.5,
  minimumSpan = 0.0001,
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

export const fitChartYRange = (
  values: readonly number[],
  paddingRatio = 0.12,
  minimumSpan = 0.1,
): ChartYRange | null => {
  const finiteValues = values.filter(Number.isFinite);
  if (finiteValues.length === 0) return null;

  const rawMinimum = Math.min(...finiteValues);
  const rawMaximum = Math.max(...finiteValues);
  const span = Math.max(minimumSpan, rawMaximum - rawMinimum);
  const minimum = Math.max(0, rawMinimum - span * paddingRatio);
  const maximum = Math.max(minimum + minimumSpan, rawMaximum + span * paddingRatio);
  return { minimum, maximum };
};

export const zoomChartYRange = (
  range: ChartYRange,
  factor: number,
  minimumSpan = 0.02,
  maximumSpan = 40,
): ChartYRange => {
  const center = (range.minimum + range.maximum) / 2;
  const currentSpan = Math.max(minimumSpan, range.maximum - range.minimum);
  const nextSpan = clampChartValue(currentSpan / factor, minimumSpan, maximumSpan);
  let minimum = center - nextSpan / 2;
  let maximum = center + nextSpan / 2;
  if (minimum < 0) {
    maximum -= minimum;
    minimum = 0;
  }
  return { minimum, maximum };
};

export const panChartYRange = (
  range: ChartYRange,
  fractionOfCurrentSpan: number,
): ChartYRange => {
  const span = Math.max(0.02, range.maximum - range.minimum);
  let minimum = range.minimum + span * fractionOfCurrentSpan;
  let maximum = range.maximum + span * fractionOfCurrentSpan;
  if (minimum < 0) {
    maximum -= minimum;
    minimum = 0;
  }
  return { minimum, maximum };
};
