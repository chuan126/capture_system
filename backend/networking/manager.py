from __future__ import annotations

import hashlib
import os
import subprocess
import tempfile
from dataclasses import dataclass


class WifiManagerError(RuntimeError):
    pass


@dataclass(frozen=True, slots=True)
class WifiNetwork:
    ssid: str
    signal: int
    security: str
    connected: bool

    @property
    def secured(self) -> bool:
        return self.security.strip() not in {"", "--"}


class NetworkManagerWifi:
    def __init__(self, timeout_seconds: float = 8.0, connect_timeout_seconds: float = 35.0) -> None:
        self.timeout_seconds = timeout_seconds
        self.connect_timeout_seconds = connect_timeout_seconds

    def status(self) -> dict[str, object]:
        try:
            interface = self._wifi_interface()
            connected_ssid = self._connected_ssid(interface)
            return {
                "available": True,
                "connected": connected_ssid is not None,
                "connected_ssid": connected_ssid,
                "detail": "" if connected_ssid else "Wi-Fi未连接",
            }
        except WifiManagerError as error:
            return {
                "available": False,
                "connected": False,
                "connected_ssid": None,
                "detail": str(error),
            }

    def list_networks(self, *, rescan: bool = False) -> list[WifiNetwork]:
        interface = self._wifi_interface()
        if rescan:
            self._request_scan(interface)
        command = [
            "nmcli", "--terse", "--escape", "yes",
            "--fields", "IN-USE,SSID,SIGNAL,SECURITY",
            "device", "wifi", "list",
            # The explicit rescan above exists to surface authorization failures.
            # Keep --rescan=yes here so nmcli waits for a fresh NetworkManager scan
            # instead of immediately returning a stale access-point cache.
            "--rescan", "yes" if rescan else "no",
            "ifname", interface,
        ]
        result = self._run(command)
        if result.returncode != 0:
            raise WifiManagerError(self._safe_detail(result, "Wi-Fi扫描失败"))
        strongest: dict[str, WifiNetwork] = {}
        for line in result.stdout.splitlines():
            fields = self._split_terse(line, 4)
            if len(fields) != 4:
                continue
            in_use, ssid, signal_text, security = fields
            ssid = ssid.strip()
            if not ssid:
                continue
            try:
                signal = max(0, min(100, int(signal_text)))
            except ValueError:
                signal = 0
            item = WifiNetwork(
                ssid=ssid,
                signal=signal,
                security=security.strip(),
                connected=in_use.strip().lower() in {"*", "yes", "true"},
            )
            current = strongest.get(ssid)
            if current is None or item.connected or item.signal > current.signal:
                strongest[ssid] = item
        return sorted(strongest.values(), key=lambda item: (not item.connected, -item.signal, item.ssid.casefold()))

    def _request_scan(self, interface: str) -> None:
        result = self._run(["nmcli", "device", "wifi", "rescan", "ifname", interface])
        if result.returncode == 0:
            return
        detail = self._safe_detail(result, "Wi-Fi扫描请求失败")
        normalized = detail.casefold()
        if "not authorized" in normalized or "not authorised" in normalized:
            raise WifiManagerError(
                "Wi-Fi扫描权限不足，请检查NetworkManager的org.freedesktop.NetworkManager.wifi.scan授权"
            )
        raise WifiManagerError(detail)

    def connect(self, ssid: str, password: str | None) -> dict[str, object]:
        clean_ssid = self._validate_ssid(ssid)
        clean_password = self._validate_password(password)
        interface = self._wifi_interface()
        networks = self.list_networks(rescan=True)
        selected = next((item for item in networks if item.ssid == clean_ssid), None)
        if selected is None:
            raise WifiManagerError("未发现所选Wi-Fi，请重新扫描")
        if selected.secured and not clean_password:
            raise WifiManagerError("该Wi-Fi需要密码")
        if not selected.secured and clean_password:
            clean_password = None

        profile = self._profile_name(clean_ssid)
        if not self._profile_exists(profile):
            result = self._run([
                "nmcli", "connection", "add", "type", "wifi", "ifname", interface,
                "con-name", profile, "ssid", clean_ssid,
            ])
            if result.returncode != 0:
                raise WifiManagerError(self._safe_detail(result, "Wi-Fi配置创建失败", clean_password))

        if selected.secured:
            result = self._run([
                "nmcli", "connection", "modify", profile,
                "wifi-sec.key-mgmt", "wpa-psk", "connection.autoconnect", "yes",
            ])
            if result.returncode != 0:
                raise WifiManagerError(self._safe_detail(result, "Wi-Fi安全配置失败", clean_password))
            with tempfile.NamedTemporaryFile("w", encoding="utf-8", prefix="capture-wifi-", delete=False) as stream:
                password_path = stream.name
                stream.write(f"wifi-sec.psk:{clean_password}\n")
            try:
                os.chmod(password_path, 0o600)
                result = self._run([
                    "nmcli", "--wait", str(int(self.connect_timeout_seconds)),
                    "connection", "up", "id", profile, "ifname", interface,
                    "passwd-file", password_path,
                ], timeout=self.connect_timeout_seconds + 3.0)
            finally:
                try:
                    os.unlink(password_path)
                except OSError:
                    pass
        else:
            result = self._run([
                "nmcli", "--wait", str(int(self.connect_timeout_seconds)),
                "connection", "up", "id", profile, "ifname", interface,
            ], timeout=self.connect_timeout_seconds + 3.0)

        if result.returncode != 0:
            raise WifiManagerError(self._safe_detail(result, "Wi-Fi连接失败", clean_password))
        connected_ssid = self._connected_ssid(interface)
        if connected_ssid != clean_ssid:
            raise WifiManagerError("NetworkManager未确认连接到所选Wi-Fi")
        return {
            "connected": True,
            "connected_ssid": connected_ssid,
        }

    def _wifi_interface(self) -> str:
        result = self._run(["nmcli", "--terse", "--escape", "yes", "--fields", "DEVICE,TYPE", "device", "status"])
        if result.returncode != 0:
            raise WifiManagerError(self._safe_detail(result, "NetworkManager不可用"))
        for line in result.stdout.splitlines():
            fields = self._split_terse(line, 2)
            if len(fields) == 2 and fields[1] == "wifi" and fields[0]:
                return fields[0]
        raise WifiManagerError("未发现由NetworkManager管理的Wi-Fi网卡")

    def _connected_ssid(self, interface: str) -> str | None:
        result = self._run([
            "nmcli", "--terse", "--escape", "yes", "--fields", "IN-USE,SSID",
            "device", "wifi", "list", "--rescan", "no", "ifname", interface,
        ])
        if result.returncode != 0:
            return None
        for line in result.stdout.splitlines():
            fields = self._split_terse(line, 2)
            if len(fields) == 2 and fields[0].strip().lower() in {"*", "yes", "true"} and fields[1].strip():
                return fields[1].strip()
        return None

    def _profile_exists(self, profile: str) -> bool:
        result = self._run(["nmcli", "--terse", "--escape", "yes", "--fields", "NAME", "connection", "show"])
        if result.returncode != 0:
            raise WifiManagerError(self._safe_detail(result, "无法读取NetworkManager连接配置"))
        return any(self._split_terse(line, 1)[0] == profile for line in result.stdout.splitlines() if line)

    def _run(self, command: list[str], *, timeout: float | None = None) -> subprocess.CompletedProcess[str]:
        try:
            return subprocess.run(
                command,
                text=True,
                capture_output=True,
                timeout=timeout or self.timeout_seconds,
                check=False,
            )
        except FileNotFoundError as error:
            raise WifiManagerError("未安装NetworkManager命令nmcli") from error
        except subprocess.TimeoutExpired as error:
            raise WifiManagerError("NetworkManager操作超时") from error

    @staticmethod
    def _profile_name(ssid: str) -> str:
        digest = hashlib.sha256(ssid.encode("utf-8")).hexdigest()[:12]
        return f"capture-wifi-{digest}"

    @staticmethod
    def _validate_ssid(ssid: str) -> str:
        value = ssid.strip()
        if not value or "\x00" in value or "\n" in value or "\r" in value:
            raise WifiManagerError("Wi-Fi名称无效")
        if len(value.encode("utf-8")) > 32:
            raise WifiManagerError("Wi-Fi名称超过32字节")
        return value

    @staticmethod
    def _validate_password(password: str | None) -> str | None:
        if password is None or password == "":
            return None
        if "\x00" in password or "\n" in password or "\r" in password:
            raise WifiManagerError("Wi-Fi密码格式无效")
        if len(password) > 128:
            raise WifiManagerError("Wi-Fi密码过长")
        return password

    @staticmethod
    def _safe_detail(result: subprocess.CompletedProcess[str], fallback: str, secret: str | None = None) -> str:
        detail = (result.stderr or result.stdout).strip() or fallback
        if secret:
            detail = detail.replace(secret, "***")
        return detail

    @staticmethod
    def _split_terse(line: str, expected: int) -> list[str]:
        values: list[str] = []
        current: list[str] = []
        escaped = False
        for char in line:
            if escaped:
                current.append(char)
                escaped = False
                continue
            if char == "\\":
                escaped = True
                continue
            if char == ":" and len(values) < expected - 1:
                values.append("".join(current))
                current = []
                continue
            current.append(char)
        if escaped:
            current.append("\\")
        values.append("".join(current))
        return values
