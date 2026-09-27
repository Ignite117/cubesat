#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <QtGlobal>

struct EPS_Telemetry {
    float bus_voltage[3];
    float bus_current[3];
    float battery_voltage;
    float battery_temp;
    float battery_current;
    uint8_t soc_percent;
    float solar_power[12];
};

struct Thermal_Telemetry {
    float temp_cpu;
    float temp_battery;
    float temp_antenna;
    float temp_in;
    float temp_solar[12];
};

struct ADCS_Telemetry {
    float gyro[3];
    float magnetometer[3];
    float sun_sensor[6];
    float quaternion[4];
    float reaction_wheel_speed[3];
};

struct OBC_Telemetry {
    uint8_t  cpu_load_percent;
    uint32_t memory_used_kb;
    uint16_t reboot_count;
    uint16_t watchdog_reset_count;
    uint16_t firmware_version;
     uint8_t last_error_code;
};

struct Comm_Telemetry {
    float   tx_power_dbm;
    int16_t rssi;
    float   bit_error_rate;
    bool    antenna_deployed;
};

struct GPS_Telemetry {
    double   latitude;
    double   longitude;
    float    altitude_km;
    float    speed_kmh;
    uint32_t gps_timestamp;
};

struct TelemetryPacket {
    uint32_t timestamp;
    EPS_Telemetry      eps;
    Thermal_Telemetry  thermal;
    ADCS_Telemetry     adcs;
    OBC_Telemetry      obc;
    Comm_Telemetry     comm;
    GPS_Telemetry      gps;
    uint16_t crc;
};

#endif // TELEMETRY_H