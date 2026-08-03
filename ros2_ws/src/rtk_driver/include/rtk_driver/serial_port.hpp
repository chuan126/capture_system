#ifndef RTK_DRIVER__SERIAL_PORT_HPP_
#define RTK_DRIVER__SERIAL_PORT_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

namespace rtk_driver
{

class SerialPort final
{
public:
  SerialPort() = default;
  ~SerialPort();

  SerialPort(const SerialPort &) = delete;
  SerialPort & operator=(const SerialPort &) = delete;

  bool open_device(const std::string & device, int baud_rate, std::string & error_message);
  void close_device();
  bool is_open() const noexcept;
  int file_descriptor() const noexcept;

  // 返回读取字节数；暂时无数据返回0；设备错误返回-1并填写错误信息。
  std::ptrdiff_t read_bytes(
    std::uint8_t * buffer,
    std::size_t capacity,
    std::string & error_message);

private:
  int file_descriptor_{-1};
};

}  // namespace rtk_driver

#endif  // RTK_DRIVER__SERIAL_PORT_HPP_
