export const PCV1_HEADER_BYTES = 24;
export const PCV1_MAX_POINTS = 10_000;

export type CloudStreamInfo = {
  type: "stream_info";
  protocol: "PCV1" | "PCV2";
  version: number;
  header_bytes: number;
  point_format: "xyz_float32_le" | "xyz_float32_class_uint8_le";
  point_stride: number;
  max_points: number;
  frame_id: string;
  coordinate_mode: "local_enu" | "sensor";
  sensor_clock: "device_boot";
  color_mode: "single" | "classification";
};

export type CloudStatusMessage = {
  type: "status";
  state:
    | "waiting"
    | "streaming"
    | "paused"
    | "degraded"
    | "ros_unavailable";
  reason: string;
  detail: string;
};

export type CloudPreviewFrame = {
  sequence: number;
  sensorStampNs: bigint;
  pointCount: number;
  positions: Float32Array;
  colors?: Float32Array;
};

export function parseCloudPreviewText(
  text: string,
): CloudStreamInfo | CloudStatusMessage {
  const message: unknown = JSON.parse(text);
  if (!message || typeof message !== "object" || !("type" in message)) {
    throw new Error("点云文本消息缺少type");
  }

  if (message.type === "stream_info") {
    const stream = message as Partial<CloudStreamInfo>;
    const pcv1Contract = stream.protocol === "PCV1" && stream.version === 1
      && stream.point_format === "xyz_float32_le" && stream.point_stride === 12
      && stream.color_mode === "single";
    const pcv2Contract = stream.protocol === "PCV2" && stream.version === 2
      && stream.point_format === "xyz_float32_class_uint8_le" && stream.point_stride === 16
      && stream.color_mode === "classification";
    if (
      (!pcv1Contract && !pcv2Contract)
      || stream.header_bytes !== PCV1_HEADER_BYTES
      || !["local_enu", "sensor"].includes(String(stream.coordinate_mode))
      || typeof stream.frame_id !== "string"
      || !stream.frame_id
      || typeof stream.max_points !== "number"
      || stream.max_points > PCV1_MAX_POINTS
    ) {
      throw new Error("点云流描述与PCV1/PCV2不兼容");
    }
    return stream as CloudStreamInfo;
  }

  if (message.type === "status") {
    const status = message as Partial<CloudStatusMessage>;
    if (
      typeof status.state !== "string"
      || typeof status.reason !== "string"
      || typeof status.detail !== "string"
    ) {
      throw new Error("点云状态消息字段不完整");
    }
    return status as CloudStatusMessage;
  }

  throw new Error("无法识别的点云文本消息");
}

export function parseCloudPreviewBinary(
  buffer: ArrayBuffer,
  maxPoints = PCV1_MAX_POINTS,
): CloudPreviewFrame {
  if (buffer.byteLength < PCV1_HEADER_BYTES) {
    throw new Error("PCV1二进制帧短于固定头");
  }

  const view = new DataView(buffer);
  const magic = String.fromCharCode(
    view.getUint8(0),
    view.getUint8(1),
    view.getUint8(2),
    view.getUint8(3),
  );
  const version = view.getUint16(4, true);
  const pointCount = view.getUint32(20, true);

  const isPcv2 = magic === "PCV2" && version === 2;
  if (!(magic === "PCV1" && version === 1) && !isPcv2) {
    throw new Error("PCV1/PCV2 magic或版本不受支持");
  }
  if (pointCount > maxPoints || pointCount > PCV1_MAX_POINTS) {
    throw new Error("PCV1点数超过首版上限");
  }

  const stride = isPcv2 ? 16 : 12;
  const expectedLength = PCV1_HEADER_BYTES + pointCount * stride;
  if (buffer.byteLength !== expectedLength) {
    throw new Error("PCV1帧长度与点数不一致");
  }

  if (!isPcv2) return {
    sequence: view.getUint32(8, true),
    sensorStampNs: view.getBigUint64(12, true),
    pointCount,
    // 固定头长度可被4整除，因此能够安全建立零额外解析的浮点视图。
    positions: new Float32Array(
      buffer,
      PCV1_HEADER_BYTES,
      pointCount * 3,
    ),
  };
  const positions = new Float32Array(pointCount * 3);
  const colors = new Float32Array(pointCount * 3);
  const palette = [
    [0x3b / 255, 0x82 / 255, 0xf6 / 255],
    [0x22 / 255, 0xc5 / 255, 0x5e / 255],
    [0xff / 255, 0x4d / 255, 0x4f / 255],
  ];
  for (let index = 0; index < pointCount; index += 1) {
    const inputOffset = PCV1_HEADER_BYTES + index * 16;
    positions[index * 3] = view.getFloat32(inputOffset, true);
    positions[index * 3 + 1] = view.getFloat32(inputOffset + 4, true);
    positions[index * 3 + 2] = view.getFloat32(inputOffset + 8, true);
    const classification = view.getUint8(inputOffset + 12);
    const color = palette[classification <= 2 ? classification : 0];
    colors.set(color, index * 3);
  }
  return {sequence: view.getUint32(8, true), sensorStampNs: view.getBigUint64(12, true), pointCount, positions, colors};
}
