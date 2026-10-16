#ifndef MQTT_CLI_H
#define MQTT_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* MQTT console replacing the MCU UART CLI.
 *
 * Topic scheme (base topic default "gbt"):
 *   <base>/cmd   (in)  command line, same syntax as the UART CLI,
 *                      e.g. "RSET SOC_PERCENT 60" or "STATE"
 *   <base>/resp  (out) response text of the last command
 *   <base>/state (out) retained GB/T 27930 charge state name (STATE=... value)
 *   <base>/can   (out) raw CAN frame trace lines while "DCAN ON" is active
 */

bool MqttCli_Init(const char *host, int port, const char *user, const char *pass,
                  const char *base_topic, const char *client_id);
bool MqttCli_Connect(void);
int  MqttCli_Socket(void);     /* fd for poll(), -1 while disconnected */
bool MqttCli_WantWrite(void);  /* pending outbound data */
void MqttCli_Loop(void);       /* service I/O + reconnect; call every loop pass */
void MqttCli_Publish(const char *subtopic, const char *payload, bool retain);
void MqttCli_Disconnect(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLI_H */
