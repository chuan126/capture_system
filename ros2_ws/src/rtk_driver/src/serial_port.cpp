#define _DEFAULT_SOURCE

#include "rtk_driver/serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace rtk_driver
{
namespace
{

bool baud_rate_to_termios(const int baud_rate, speed_t & speed)
{
  switch (baud_rate) {
    case 9600:
      speed = B9600;
      return true;
    case 38400:
      speed = B38400;
      return true;
    case 57600:
      speed = B57600;
      return true;
    case 115200:
      speed = B115200;
      return true;
    case 230400:
      speed = B230400;
      return true;
    default:
      return false;
  }
}

}  // namespace

SerialPort::~SerialPort()
{
  close_device();
}

bool SerialPort::open_device(
  const std::string & device,
  const int baud_rate,
  std::string & error_message)
{
  close_device();

  speed_t speed{};
  if (!baud_rate_to_termios(baud_rate, speed)) {
    error_message = "不支持的波特率：" + std::to_string(baud_rate);
    return false;
  }

  const int descriptor = ::open(device.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
  if (descriptor < 0) {
    error_message = std::strerror(errno);
    return false;
  }

  termios tty{};
  if (tcgetattr(descriptor, &tty) != 0) {
    error_message = std::strerror(errno);
    ::close(descriptor);
    return false;
  }

  cfmakeraw(&tty);
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);
  tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS | CSIZE | HUPCL);
  tty.c_cflag |= CS8 | CLOCAL | CREAD;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(descriptor, TCSANOW, &tty) != 0) {
    error_message = std::strerror(errno);
    ::close(descriptor);
    return false;
  }

  file_descriptor_ = descriptor;
  error_message.clear();
  return true;
}

void SerialPort::close_device()
{
  if (file_descriptor_ >= 0) {
    ::close(file_descriptor_);
    file_descriptor_ = -1;
  }
}

bool SerialPort::is_open() const noexcept
{
  return file_descriptor_ >= 0;
}

int SerialPort::file_descriptor() const noexcept
{
  return file_descriptor_;
}

std::ptrdiff_t SerialPort::read_bytes(
  std::uint8_t * buffer,
  const std::size_t capacity,
  std::string & error_message)
{
  pollfd descriptor_status{};
  descriptor_status.fd = file_descriptor_;
  descriptor_status.events = POLLIN;
  const int poll_result = ::poll(&descriptor_status, 1, 0);
  if (poll_result < 0 && errno != EINTR) {
    error_message = std::strerror(errno);
    return -1;
  }
  if (poll_result > 0 &&
    (descriptor_status.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
  {
    error_message = "串口连接已断开";
    return -1;
  }

  const ssize_t length = ::read(file_descriptor_, buffer, capacity);
  if (length >= 0) {
    error_message.clear();
    return static_cast<std::ptrdiff_t>(length);
  }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    error_message.clear();
    return 0;
  }

  error_message = std::strerror(errno);
  return -1;
}

}  // namespace rtk_driver
