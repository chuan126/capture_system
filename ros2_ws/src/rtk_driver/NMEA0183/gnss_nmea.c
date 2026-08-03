#include "gnss_nmea.h"

#include <math.h>
#include <string.h>

#define GNSS_NMEA_BUFFER_SIZE 1024U
#define GNSS_NMEA_FRAME_LIMIT 350U

/* 四组接收状态和缓冲区。 */
static unsigned short GPRMCCount;
static unsigned short GPGGACount;
static unsigned char GPRMCBuf[GNSS_NMEA_BUFFER_SIZE];
static unsigned char GPGGABuf[GNSS_NMEA_BUFFER_SIZE];
static unsigned short HandleGPRMCCount;
static unsigned char GPRMCFlag;
static unsigned short HandleGPGGACount;
static unsigned char GPGGAFlag;

static unsigned short GPGSACount;
static unsigned char GPGSABuf[GNSS_NMEA_BUFFER_SIZE];
static unsigned short HandleGPGSACount;
static unsigned char GPGSAFlag;

static unsigned short BESTPOSACount;
static unsigned char BESTPOSABuf[GNSS_NMEA_BUFFER_SIZE];
static unsigned short HandleBESTPOSACount;
static unsigned char BESTPOSAFlag;

double GPSLatitude_Cal;
double GPSLongitude_Cal;
double GPSHeight_Cal;
float GPSHDOP_Cal;
char GPSStarNum_Cal;
_gnssout_data m_GnssData;

static unsigned char Decode_GNSS_GPRMC(void);
static unsigned char Decode_GNSS_GPGGA(void);
static unsigned char Decode_GNSS_GPGSA(void);
static unsigned char Decode_GNSS_BESTPOSA(void);
static unsigned char Function_ASCLL_EX_DECIMAl(char INS_Message);
static void seek_comma(unsigned char *buf, int length, int *pos_comma);
static int seek_dot(unsigned char *buf, int length);
static unsigned char HEXtoCHAR(int data);

static void reset_gprmc(void)
{
    GPRMCCount = 0U;
    HandleGPRMCCount = 0U;
    GPRMCFlag = 0U;
}

static void reset_gpgga(void)
{
    GPGGACount = 0U;
    HandleGPGGACount = 0U;
    GPGGAFlag = 0U;
}

static void reset_gpgsa(void)
{
    GPGSACount = 0U;
    HandleGPGSACount = 0U;
    GPGSAFlag = 0U;
}

static void reset_bestposa(void)
{
    BESTPOSACount = 0U;
    HandleBESTPOSACount = 0U;
    BESTPOSAFlag = 0U;
}

void gnss_nmea_init(void)
{
    reset_gprmc();
    reset_gpgga();
    reset_gpgsa();
    reset_bestposa();

    memset(GPRMCBuf, 0, sizeof(GPRMCBuf));
    memset(GPGGABuf, 0, sizeof(GPGGABuf));
    memset(GPGSABuf, 0, sizeof(GPGSABuf));
    memset(BESTPOSABuf, 0, sizeof(BESTPOSABuf));
    memset(&m_GnssData, 0, sizeof(m_GnssData));

    GPSLatitude_Cal = 0.0;
    GPSLongitude_Cal = 0.0;
    GPSHeight_Cal = 0.0;
    GPSHDOP_Cal = 0.0F;
    GPSStarNum_Cal = 0;
}

gnss_nmea_event_t gnss_nmea_input_byte(uint8_t byte)
{
    unsigned char temp = byte;
    uint32_t event = GNSS_NMEA_EVENT_NONE;

    /* GPRMC */
    switch (GPRMCCount)
    {
        case 0:
            if (temp == '$')
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        case 1:
            if ((temp == 'G') || (temp == 'B'))
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        case 2:
            if ((temp == 'P') || (temp == 'N') || (temp == 'D'))
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        case 3:
            if (temp == 'R')
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        case 4:
            if (temp == 'M')
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        case 5:
            if (temp == 'C')
            {
                GPRMCBuf[GPRMCCount] = temp;
                GPRMCCount++;
            }
            else
            {
                GPRMCCount = 0U;
            }
            break;

        default:
            GPRMCBuf[GPRMCCount] = temp;
            GPRMCCount++;
            if (temp == '*')
            {
                HandleGPRMCCount = (unsigned short)(GPRMCCount + 2U);
                GPRMCFlag = 1U;
            }
            if ((GPRMCFlag == 1U)
                && (GPRMCCount > (unsigned short)(HandleGPRMCCount - 1U)))
            {
                if (Decode_GNSS_GPRMC() != 0U)
                {
                    event |= GNSS_NMEA_EVENT_RMC_OK;
                }
                else
                {
                    event |= GNSS_NMEA_EVENT_CHECKSUM_ERROR;
                }
                reset_gprmc();
            }
            if (GPRMCCount >= GNSS_NMEA_FRAME_LIMIT)
            {
                reset_gprmc();
                event |= GNSS_NMEA_EVENT_FRAME_OVERFLOW;
            }
            break;
    }

    /* GPGGA */
    switch (GPGGACount)
    {
        case 0:
            if (temp == '$')
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        case 1:
            if ((temp == 'G') || (temp == 'B'))
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        case 2:
            if ((temp == 'P') || (temp == 'N') || (temp == 'D'))
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        case 3:
            if (temp == 'G')
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        case 4:
            if (temp == 'G')
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        case 5:
            if (temp == 'A')
            {
                GPGGABuf[GPGGACount] = temp;
                GPGGACount++;
                event |= GNSS_NMEA_EVENT_GGA_HEADER;
            }
            else
            {
                GPGGACount = 0U;
            }
            break;

        default:
            GPGGABuf[GPGGACount] = temp;
            GPGGACount++;
            if ((temp == '*') && (GPGGACount > 20U))
            {
                HandleGPGGACount = (unsigned short)(GPGGACount + 2U);
                GPGGAFlag = 1U;
            }
            if ((GPGGAFlag == 1U)
                && (GPGGACount > (unsigned short)(HandleGPGGACount - 1U)))
            {
                if (Decode_GNSS_GPGGA() != 0U)
                {
                    event |= GNSS_NMEA_EVENT_GGA_OK;
                }
                else
                {
                    event |= GNSS_NMEA_EVENT_CHECKSUM_ERROR;
                }
                reset_gpgga();
            }
            if (GPGGACount >= GNSS_NMEA_FRAME_LIMIT)
            {
                reset_gpgga();
                event |= GNSS_NMEA_EVENT_FRAME_OVERFLOW;
            }
            break;
    }

    /* GPGSA */
    switch (GPGSACount)
    {
        case 0:
            if (temp == '$')
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        case 1:
            if ((temp == 'G') || (temp == 'B'))
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        case 2:
            if ((temp == 'P') || (temp == 'N') || (temp == 'B')
                || (temp == 'L') || (temp == 'D'))
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        case 3:
            if (temp == 'G')
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        case 4:
            if (temp == 'S')
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        case 5:
            if (temp == 'A')
            {
                GPGSABuf[GPGSACount] = temp;
                GPGSACount++;
            }
            else
            {
                GPGSACount = 0U;
            }
            break;

        default:
            GPGSABuf[GPGSACount] = temp;
            GPGSACount++;
            if ((temp == '*') && (GPGSACount > 20U))
            {
                HandleGPGSACount = (unsigned short)(GPGSACount + 2U);
                GPGSAFlag = 1U;
            }
            if ((GPGSAFlag == 1U)
                && (GPGSACount > (unsigned short)(HandleGPGSACount - 1U)))
            {
                if (Decode_GNSS_GPGSA() != 0U)
                {
                    event |= GNSS_NMEA_EVENT_GSA_OK;
                }
                else
                {
                    event |= GNSS_NMEA_EVENT_CHECKSUM_ERROR;
                }
                reset_gpgsa();
            }
            if (GPGSACount >= GNSS_NMEA_FRAME_LIMIT)
            {
                reset_gpgsa();
                event |= GNSS_NMEA_EVENT_FRAME_OVERFLOW;
            }
            break;
    }

    /* BESTPOSA */
    switch (BESTPOSACount)
    {
        case 0:
            if (temp == '#')
            {
                BESTPOSABuf[BESTPOSACount++] = temp;
            }
            else
            {
                BESTPOSACount = 0U;
            }
            break;
        case 1:
            if (temp == 'B') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 2:
            if (temp == 'E') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 3:
            if (temp == 'S') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 4:
            if (temp == 'T') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 5:
            if (temp == 'P') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 6:
            if (temp == 'O') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 7:
            if (temp == 'S') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        case 8:
            if (temp == 'A') BESTPOSABuf[BESTPOSACount++] = temp;
            else BESTPOSACount = 0U;
            break;
        default:
            BESTPOSABuf[BESTPOSACount] = temp;
            BESTPOSACount++;
            if ((temp == '*') && (BESTPOSACount > 20U))
            {
                HandleBESTPOSACount = (unsigned short)(BESTPOSACount + 2U);
                BESTPOSAFlag = 1U;
            }
            if ((BESTPOSAFlag == 1U)
                && (BESTPOSACount > (unsigned short)(HandleBESTPOSACount - 1U)))
            {
                if (Decode_GNSS_BESTPOSA() != 0U)
                {
                    event |= GNSS_NMEA_EVENT_BESTPOSA_OK;
                }
                reset_bestposa();
            }
            if (BESTPOSACount >= GNSS_NMEA_FRAME_LIMIT)
            {
                reset_bestposa();
                event |= GNSS_NMEA_EVENT_FRAME_OVERFLOW;
            }
            break;
    }

    return (gnss_nmea_event_t)event;
}

uint32_t gnss_nmea_input(const uint8_t *data, size_t length)
{
    uint32_t event = GNSS_NMEA_EVENT_NONE;
    size_t i;

    if ((data == NULL) && (length > 0U))
    {
        return GNSS_NMEA_EVENT_NONE;
    }

    for (i = 0U; i < length; i++)
    {
        event |= (uint32_t)gnss_nmea_input_byte(data[i]);
    }

    return event;
}

const gnss_nmea_data_t *gnss_nmea_get_data(void)
{
    return &m_GnssData;
}

double gnss_nmea_get_latitude(void)
{
    return GPSLatitude_Cal;
}

double gnss_nmea_get_longitude(void)
{
    return GPSLongitude_Cal;
}

double gnss_nmea_get_height(void)
{
    return GPSHeight_Cal;
}

static unsigned char checksum_ok(
    unsigned char *buf,
    unsigned short length)
{
    unsigned char checksum = 0U;
    unsigned char checksum_l;
    unsigned char checksum_h;
    int i;

    for (i = 1; i < ((int)length - 3); i++)
    {
        checksum = (unsigned char)(checksum ^ buf[i]);
    }
    checksum_l = HEXtoCHAR(checksum & 0x0FU);
    checksum_h = HEXtoCHAR((checksum & 0xF0U) >> 4);

    return (unsigned char)(
        (buf[length - 2U] == checksum_h)
        && (buf[length - 1U] == checksum_l));
}

static double original_decimal(
    unsigned char *buf,
    unsigned int length)
{
    double value = 0.0;
    int dot;
    int i;
    int power;

    dot = seek_dot(buf, (int)length);
    for (i = 0; i < (int)length; i++)
    {
        if (buf[i] != '.')
        {
            buf[i] = Function_ASCLL_EX_DECIMAl((char)buf[i]);
        }
    }
    for (i = 0; i < dot; i++)
    {
        value += buf[i] * (int)pow(10.0, (dot - 1) - i);
    }
    for (i = dot + 1; i < (int)length; i++)
    {
        power = (int)pow(10.0, i - dot);
        value += 1.0 * buf[i] / power;
    }
    return value;
}

static unsigned int prepare_fields(
    unsigned char *buf,
    unsigned short length,
    unsigned char fields[][20],
    unsigned int field_length[],
    unsigned int field_count)
{
    int pos_comma[40] = {0};
    unsigned int i;

    seek_comma(buf, (int)length, pos_comma);
    for (i = 0U; i < field_count; i++)
    {
        if (pos_comma[i + 1U] > 0)
        {
            if (pos_comma[i + 1U] - pos_comma[i] == 1)
            {
                field_length[i] = 0U;
            }
            else
            {
                field_length[i] =
                    (unsigned int)(pos_comma[i + 1U]
                    - pos_comma[i] - 1);
            }
        }
        if ((field_length[i] > 0U) && (field_length[i] < 20U))
        {
            memcpy(
                fields[i],
                buf + pos_comma[i] + 1,
                field_length[i]);
        }
    }
    return field_count;
}

static unsigned char Decode_GNSS_GPRMC(void)
{
    unsigned char Gnss_ctemp[11][20] = {{0U}};
    unsigned int Gnss_dataLength[11] = {0U};
    unsigned int i;

    if (checksum_ok(GPRMCBuf, HandleGPRMCCount) == 0U)
    {
        return 0U;
    }

    (void)prepare_fields(
        GPRMCBuf,
        HandleGPRMCCount,
        Gnss_ctemp,
        Gnss_dataLength,
        11U);

    if (Gnss_dataLength[0] >= 6U)
    {
        for (i = 0U; i < Gnss_dataLength[0]; i++)
        {
            if (Gnss_ctemp[0][i] != '.')
            {
                Gnss_ctemp[0][i] =
                    Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[0][i]);
            }
        }
        m_GnssData.Gnss_UTC_Hour =
            10U * Gnss_ctemp[0][0] + Gnss_ctemp[0][1];
        m_GnssData.Gnss_UTC_Min =
            10U * Gnss_ctemp[0][2] + Gnss_ctemp[0][3];
        m_GnssData.Gnss_UTC_Second =
            10U * Gnss_ctemp[0][4] + Gnss_ctemp[0][5];
    }

    if (Gnss_dataLength[1] == 1U)
    {
        memcpy(&m_GnssData.isGnssVaild, Gnss_ctemp[1], 1U);
    }
    else
    {
        m_GnssData.isGnssVaild = 0;
    }

    if ((Gnss_dataLength[6] > 0U) && (Gnss_dataLength[6] < 20U))
    {
        m_GnssData.Gnss_Velocity =
            (float)original_decimal(Gnss_ctemp[6], Gnss_dataLength[6]);
    }

    if (Gnss_dataLength[8] >= 6U)
    {
        for (i = 0U; i < Gnss_dataLength[8]; i++)
        {
            Gnss_ctemp[8][i] =
                Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[8][i]);
        }
        m_GnssData.Gnss_UTC_Day =
            10U * Gnss_ctemp[8][0] + Gnss_ctemp[8][1];
        m_GnssData.Gnss_UTC_Month =
            10U * Gnss_ctemp[8][2] + Gnss_ctemp[8][3];
        m_GnssData.Gnss_UTC_Year =
            10U * Gnss_ctemp[8][4] + Gnss_ctemp[8][5];
    }

    return 1U;
}

static unsigned char Decode_GNSS_GPGGA(void)
{
    unsigned char Gnss_ctemp[14][20] = {{0U}};
    unsigned int Gnss_dataLength[14] = {0U};
    int dot;
    int i;
    int power;

    if (checksum_ok(GPGGABuf, HandleGPGGACount) == 0U)
    {
        return 0U;
    }

    (void)prepare_fields(
        GPGGABuf,
        HandleGPGGACount,
        Gnss_ctemp,
        Gnss_dataLength,
        13U);

    if ((Gnss_dataLength[1] > 0U) && (Gnss_dataLength[1] < 20U))
    {
        m_GnssData.Gnss_Latitude_degree = 0U;
        m_GnssData.Gnss_Latitude_min = 0.0;
        dot = seek_dot(Gnss_ctemp[1], (int)Gnss_dataLength[1]);
        for (i = 0; i < (int)Gnss_dataLength[1]; i++)
        {
            if (Gnss_ctemp[1][i] != '.')
                Gnss_ctemp[1][i] =
                    Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[1][i]);
        }
        for (i = 0; i < dot - 2; i++)
            m_GnssData.Gnss_Latitude_degree +=
                Gnss_ctemp[1][i] * (int)pow(10.0, (dot - 3) - i);
        m_GnssData.Gnss_Latitude_min =
            Gnss_ctemp[1][dot - 2] * 10.0 + Gnss_ctemp[1][dot - 1];
        for (i = dot + 1; i < (int)Gnss_dataLength[1]; i++)
        {
            power = (int)pow(10.0, i - dot);
            m_GnssData.Gnss_Latitude_min +=
                1.0 * Gnss_ctemp[1][i] / power;
        }
    }
    GPSLatitude_Cal =
        m_GnssData.Gnss_Latitude_min / 60.0
        + m_GnssData.Gnss_Latitude_degree;

    if ((Gnss_dataLength[3] > 0U) && (Gnss_dataLength[3] < 20U))
    {
        m_GnssData.Gnss_Longitude_degree = 0U;
        m_GnssData.Gnss_Longitude_min = 0.0;
        dot = seek_dot(Gnss_ctemp[3], (int)Gnss_dataLength[3]);
        for (i = 0; i < (int)Gnss_dataLength[3]; i++)
        {
            if (Gnss_ctemp[3][i] != '.')
                Gnss_ctemp[3][i] =
                    Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[3][i]);
        }
        for (i = 0; i < dot - 2; i++)
            m_GnssData.Gnss_Longitude_degree +=
                Gnss_ctemp[3][i] * (int)pow(10.0, (dot - 3) - i);
        m_GnssData.Gnss_Longitude_min =
            Gnss_ctemp[3][dot - 2] * 10.0 + Gnss_ctemp[3][dot - 1];
        for (i = dot + 1; i < (int)Gnss_dataLength[3]; i++)
        {
            power = (int)pow(10.0, i - dot);
            m_GnssData.Gnss_Longitude_min +=
                1.0 * Gnss_ctemp[3][i] / power;
        }
    }
    GPSLongitude_Cal =
        m_GnssData.Gnss_Longitude_min / 60.0
        + m_GnssData.Gnss_Longitude_degree;

    m_GnssData.GPS_state = 0U;
    if (Gnss_dataLength[5] == 1U)
    {
        Gnss_ctemp[5][0] =
            Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[5][0]);
        memcpy(&m_GnssData.GPS_state, Gnss_ctemp[5], 1U);
    }

    m_GnssData.StarNum = 0;
    if ((Gnss_dataLength[6] > 0U) && (Gnss_dataLength[6] < 20U))
    {
        for (i = 0; i < (int)Gnss_dataLength[6]; i++)
            Gnss_ctemp[6][i] =
                Function_ASCLL_EX_DECIMAl((char)Gnss_ctemp[6][i]);
        for (i = 0; i < 2; i++)
            m_GnssData.StarNum = (char)(
                Gnss_ctemp[6][i] * (int)pow(10.0, 1 - i)
                + m_GnssData.StarNum);
    }
    GPSStarNum_Cal = m_GnssData.StarNum;

    m_GnssData.HDOP = 0.0F;
    if ((Gnss_dataLength[7] > 0U) && (Gnss_dataLength[7] < 20U))
        m_GnssData.HDOP =
            (float)original_decimal(Gnss_ctemp[7], Gnss_dataLength[7]);
    GPSHDOP_Cal = m_GnssData.HDOP;

    if ((Gnss_dataLength[8] > 0U) && (Gnss_dataLength[8] < 20U))
        m_GnssData.GNSS_High =
            (float)original_decimal(Gnss_ctemp[8], Gnss_dataLength[8]);
    GPSHeight_Cal = m_GnssData.GNSS_High;

    return 1U;
}

static unsigned char Decode_GNSS_GPGSA(void)
{
    unsigned char Gnss_ctemp[19][20] = {{0U}};
    unsigned int Gnss_dataLength[19] = {0U};

    if (checksum_ok(GPGSABuf, HandleGPGSACount) == 0U)
    {
        return 0U;
    }
    (void)prepare_fields(
        GPGSABuf,
        HandleGPGSACount,
        Gnss_ctemp,
        Gnss_dataLength,
        17U);

    m_GnssData.PDOP = 0.0F;
    if ((Gnss_dataLength[14] > 0U) && (Gnss_dataLength[14] < 20U))
        m_GnssData.PDOP =
            (float)original_decimal(Gnss_ctemp[14], Gnss_dataLength[14]);
    m_GnssData.GPSFlag = 1;
    return 1U;
}

static unsigned char Decode_GNSS_BESTPOSA(void)
{
    unsigned char Gnss_ctemp[30][20] = {{0U}};
    unsigned int Gnss_dataLength[40] = {0U};

    
    (void)prepare_fields(
        BESTPOSABuf,
        HandleBESTPOSACount,
        Gnss_ctemp,
        Gnss_dataLength,
        29U);

    m_GnssData.lat_sigma = 0.0F;
    if ((Gnss_dataLength[16] > 0U) && (Gnss_dataLength[16] < 20U))
        m_GnssData.lat_sigma =
            (float)original_decimal(Gnss_ctemp[16], Gnss_dataLength[16]);
    m_GnssData.lon_sigma = 0.0F;
    if ((Gnss_dataLength[17] > 0U) && (Gnss_dataLength[17] < 20U))
        m_GnssData.lon_sigma =
            (float)original_decimal(Gnss_ctemp[17], Gnss_dataLength[17]);
    m_GnssData.height_sigma = 0.0F;
    if ((Gnss_dataLength[18] > 0U) && (Gnss_dataLength[18] < 20U))
        m_GnssData.height_sigma =
            (float)original_decimal(Gnss_ctemp[18], Gnss_dataLength[18]);
    return 1U;
}

static unsigned char Function_ASCLL_EX_DECIMAl(char INS_Message)
{
    if ((INS_Message >= 0x30) && (INS_Message <= 0x39))
        INS_Message = (char)(INS_Message - 0x30);
    if ((INS_Message >= 0x61) && (INS_Message <= 0x7A))
        INS_Message = (char)(INS_Message - 0x57);
    if ((INS_Message >= 0x41) && (INS_Message <= 0x5A))
        INS_Message = (char)(INS_Message - 0x37);
    return (unsigned char)INS_Message;
}

static void seek_comma(unsigned char *buf, int length, int *pos_comma)
{
    int i;
    int j = 0;
    for (i = 0; i < length; i++)
    {
        if ((buf[i] == ',') || (buf[i] == ';'))
        {
            pos_comma[j] = i;
            j++;
        }
    }
}

static int seek_dot(unsigned char *buf, int length)
{
    int i;
    int pos_dot = 0;
    if (length == 1)
        return 0;
    for (i = 0; i < length; i++)
    {
        if (buf[i] == '.')
            pos_dot = i;
    }
    return pos_dot;
}

static unsigned char HEXtoCHAR(int data)
{
    unsigned char ch = 0U;
    if ((data >= 0) && (data <= 9))
        ch = (unsigned char)(data + 48);
    if ((data >= 10) && (data <= 15))
        ch = (unsigned char)(data + 55);
    return ch;
}
