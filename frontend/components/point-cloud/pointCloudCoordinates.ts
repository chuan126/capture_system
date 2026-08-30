export function mapSensorPointToDisplay(x: number, y: number, z: number): [number, number, number] {
  return [y === 0 ? 0 : -y, z === 0 ? 0 : -z, x];
}
