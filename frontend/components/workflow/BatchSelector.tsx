"use client";

import type { CaptureBatch } from "./batchModel";
import { batchStatusLabels, formatStorageBytes } from "./batchModel";

type Props = {
  batches: CaptureBatch[];
  selectedBatchId: string | null;
  onSelectBatch: (batchId: string) => void;
  label?: string;
};

export default function BatchSelector({ batches, selectedBatchId, onSelectBatch, label = "作业批次" }: Props) {
  const selected = batches.find((batch) => batch.batchId === selectedBatchId) ?? null;
  return (
    <label className="batch-selector">
      <span>{label}</span>
      <select
        value={selectedBatchId ?? ""}
        onChange={(event) => event.target.value && onSelectBatch(event.target.value)}
        disabled={batches.length === 0}
      >
        {batches.length === 0 && <option value="">暂无作业批次</option>}
        {batches.map((batch) => (
          <option key={batch.batchId} value={batch.batchId}>
            {batch.batchCode} · {batchStatusLabels[batch.status]} · {batch.visibleTaskCount} 项
          </option>
        ))}
      </select>
      {selected && (
        <small>
          {selected.operationDate} · {selected.status === "purged"
            ? `已清理 ${formatStorageBytes(selected.purgedBytes)}`
            : formatStorageBytes(selected.measurementBytes)}
        </small>
      )}
    </label>
  );
}
