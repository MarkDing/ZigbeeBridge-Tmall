// This callback file is created for your convenience. You may add application
// code to this file. If you regenerate this file over a previous version, the
// previous version will be overwritten and any code you have added will be
// lost.
#include "app/framework/include/af.h"
#include "app/framework/include/af.h"
#include "app/framework/plugin/network-creator-security/network-creator-security.h"
#include "app/framework/plugin/network-creator/network-creator.h"
#include EMBER_AF_API_NETWORK_STEERING
VAR_AT_SEGMENT(uint8_t emAvailableHeap[65536], ".heap");
#include <ctype.h>
#include <stdio.h>
#include "app/framework/plugin/device-table/device-table.h"
#include "nvm3.h"
#include "nvm3_hal_flash.h"
#include "cJSON.h"

#define transientKeyTimeoutMS() \
  (emberTransientKeyTimeoutS * MILLISECOND_TICKS_PER_SECOND)

extern EmberAfPluginDeviceTableDeviceState getCurrentState(EmberNodeId nodeId);
extern uint16_t getLastDeviceId(EmberNodeId nodeId);

// COM port for APP communication
#define APP_COM comPortUsart3

/*
  Definition of CMD and RSP
  Packet: LEN&0xFFFF, LEN^0xFFFF, Payload(JSON)
*/
#define CMD_ADD_DEV 0x41
#define CMD_REMOVE_DEV 0x42
#define CMD_SET_PROPERTY 0x43
#define CMD_GET_PROPERTY 0x44

#define RSP_DEV_ADDED 0x61
#define RSP_DEV_REMOVED 0x62
#define RSP_DEV_PROPERTY 0x63

#define RSP_ERROR 0x71
#define RSP_SUCCESS 0x72

#define LED_BLINK_PERIOD_MS 2000

#define SUBDEV_STATE_PERIOD_MS 30000

static uint8_t mostRecentButton;
static uint32_t networkOpenTimeMS = 0, networkCloseTimeMS = 0;

EmberEventControl commissioningEventControl;
EmberEventControl ledEventControl;
EmberEventControl subdevStateEventControl;

uint8_t rx_buf[256];

#define EUI64_STRING_LENGTH 19 // "0x" + 16 characters + NULL
static char eui64String[EUI64_STRING_LENGTH] = {0};
char *convert_eui64_to_string(EmberEUI64 eui64)
{
  sprintf(eui64String, "%02X%02X%02X%02X%02X%02X%02X%02X",
          eui64[7],
          eui64[6],
          eui64[5],
          eui64[4],
          eui64[3],
          eui64[2],
          eui64[1],
          eui64[0]);
  return eui64String;
}

void sendResponsePacket(uint8_t cmd, EmberNodeId node_id)
{
  char *device_name = NULL;
  char *out;
  uint8_t online, state;
  uint16_t len, tmp, dev_id;
  EmberEUI64 eui64;

  state = getCurrentState(node_id);
  online = (state == EMBER_AF_PLUGIN_DEVICE_TABLE_STATE_JOINED) ? 1 : 0;

  emberAfDeviceTableGetEui64FromNodeId(node_id, eui64);
  dev_id = getLastDeviceId(node_id);
  device_name = convert_eui64_to_string(eui64);

  cJSON *response = NULL;
  response = cJSON_CreateObject();
  if (response == NULL)
  {
    emberAfCorePrintln("No Enough Memeory\r");
    cJSON_Delete(response);
    return;
  }

  cJSON_AddNumberToObject(response, "cmd", cmd);
  cJSON_AddNumberToObject(response, "online", online);
  cJSON_AddNumberToObject(response, "model", dev_id);
  cJSON_AddStringToObject(response, "dn", device_name);

  out = cJSON_PrintUnformatted(response);
  if (out == NULL)
  {
    emberAfCorePrintln("No Enough Memory\r");
    cJSON_Delete(response);
    return;
  }
  emberAfCorePrintln("out: %s", out);

  len = strlen(out);
  tmp = len ^ 0xFFFF;
  emberSerialWriteData(APP_COM, (uint8_t *)(&len), 2);
  emberSerialWriteData(APP_COM, (uint8_t *)(&tmp), 2);
  emberSerialWriteData(APP_COM, (uint8_t *)out, len);
  cJSON_free(out);
  cJSON_Delete(response);
  return;
}

void subdevStateEventHandler(void)
{
  uint16_t node_id;
  static uint16_t prev_node_id = EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID;
  static uint16_t dev_idx = 0;

  emberEventControlSetInactive(subdevStateEventControl);
  halToggleLed(BOARDLED0);

  do
  {
    node_id = emberAfDeviceTableGetNodeIdFromIndex(dev_idx++);
    if ((node_id == EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID) ||
        (dev_idx >= EMBER_AF_PLUGIN_DEVICE_TABLE_DEVICE_TABLE_SIZE))
    {
      //    		emberAfCorePrintln("Reach end of DT. dev_idx= %d\r",dev_idx);
      dev_idx = 0;
      prev_node_id = EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_NODE_ID;
    }

    /* Same device has same node id. We only interact with different end device */
    if (node_id != prev_node_id)
    {
      prev_node_id = node_id;
      break;
    }
  } while (1);

  //	emberAfCorePrintln("Check dev_idx = %d\r",dev_idx);
  emberAfFillCommandIdentifyClusterIdentifyQuery();
  emberAfDeviceTableCommandIndexSend(dev_idx);
  emberEventControlSetDelayMS(subdevStateEventControl, SUBDEV_STATE_PERIOD_MS);
}

/** @brief NewDevice
 *
 * This callback is called when a new device joins the gateway.
 *
 * @param uui64   Ver.: always
 */
void emberAfPluginDeviceTableNewDeviceCallback(EmberEUI64 eui64)
{
  uint16_t dev_idx, node_id, dev_id;

  emberAfCorePrintln("NewDeviceCallback, EUI:%s\r", convert_eui64_to_string(eui64));

  dev_idx = emberAfDeviceTableGetFirstIndexFromEui64(eui64);
  node_id = emberAfDeviceTableGetNodeIdFromIndex(dev_idx);

  /* Same device has same node id. We only interact with different end device */
  emberAfCorePrintln("New node id = %2x\r", node_id);
  dev_id = getLastDeviceId(node_id);
  emberAfCorePrintln("Device id = %2x\r", dev_id);
  sendResponsePacket(RSP_DEV_ADDED, node_id);
  return;
}

/*
 *
 * Called when a device leaves.
 *
 *@param eui64:  EUI64 of the device that left.
 */
void emberAfPluginDeviceTableDeviceLeftCallback(EmberEUI64 eui64)
{
  uint16_t dev_idx, node_id, dev_id;
  emberAfCorePrintln("DeviceLeftCallback, EUI:%s\r", convert_eui64_to_string(eui64));

  dev_idx = emberAfDeviceTableGetFirstIndexFromEui64(eui64);
  node_id = emberAfDeviceTableGetNodeIdFromIndex(dev_idx);

  /* Same device has same node id. We only interact with different end device */
  emberAfCorePrintln("New node id=%2x\r", node_id);
  sendResponsePacket(RSP_DEV_REMOVED, node_id);
  return;
}

void commissioningEventHandler(void)
{
  EmberStatus status;

  emberAfCorePrintln("commissioningEventHandler\n\r");
  emberEventControlSetInactive(commissioningEventControl);

  if (emberAfNetworkState() == EMBER_JOINED_NETWORK)
  {
    if (mostRecentButton == BUTTON0)
    {
      status = emberAfPluginNetworkCreatorSecurityOpenNetwork();
      emberAfCorePrintln("%p network: 0x%X", "Open", status);
      if (status == EMBER_SUCCESS)
      {
        networkOpenTimeMS = halCommonGetInt32uMillisecondTick();
        networkCloseTimeMS = networkOpenTimeMS + transientKeyTimeoutMS();
        emberEventControlSetDelayMS(ledEventControl, LED_BLINK_PERIOD_MS << 1);
      }
    }
    else if (mostRecentButton == BUTTON1)
    {
      status = emberAfPluginNetworkCreatorSecurityCloseNetwork();
      networkOpenTimeMS = networkCloseTimeMS = 0;
      emberAfCorePrintln("%p network: 0x%X", "Close", status);
    }
  }
  else
  {
    status = emberAfPluginNetworkCreatorStart(true); // centralized
    emberAfCorePrintln("%p network %p: 0x%X",
                       "Form centralized",
                       "start",
                       status);
  }
}

/** @brief On/off Cluster Server Attribute Changed
 *
 * Server Attribute Changed
 *
 * @param endpoint Endpoint that is being initialized  Ver.: always
 * @param attributeId Attribute that changed  Ver.: always
 */
void emberAfOnOffClusterServerAttributeChangedCallback(int8u endpoint,
                                                       EmberAfAttributeId attributeId)
{
  // When the on/off attribute changes, set the LED appropriately.  If an error
  // occurs, ignore it because there's really nothing we can do.
  if (attributeId == ZCL_ON_OFF_ATTRIBUTE_ID)
  {
    bool onOff;
    if (emberAfReadServerAttribute(endpoint,
                                   ZCL_ON_OFF_CLUSTER_ID,
                                   ZCL_ON_OFF_ATTRIBUTE_ID,
                                   (uint8_t *)&onOff,
                                   sizeof(onOff)) == EMBER_ZCL_STATUS_SUCCESS)
    {
      if (onOff)
      {
        halSetLed(BOARDLED0);
      }
      else
      {
        halClearLed(BOARDLED0);
      }
    }
  }
}

/** @brief Complete
 *
 * This callback notifies the user that the network creation process has
 * completed successfully.
 *
 * @param network The network that the network creator plugin successfully
 * formed. Ver.: always
 * @param usedSecondaryChannels Whether or not the network creator wants to
 * form a network on the secondary channels Ver.: always
 */
void emberAfPluginNetworkCreatorCompleteCallback(const EmberNetworkParameters *network,
                                                 bool usedSecondaryChannels)
{
  emberAfCorePrintln("%p network %p: 0x%X",
                     "Form centralized",
                     "complete",
                     EMBER_SUCCESS);

  emberEventControlSetActive(ledEventControl);
}

void ledEventHandler(void)
{
  emberEventControlSetInactive(ledEventControl);

  if (emberAfNetworkState() == EMBER_JOINED_NETWORK)
  {
    uint32_t now = halCommonGetInt32uMillisecondTick();
    if (((networkOpenTimeMS < networkCloseTimeMS) && (now > networkOpenTimeMS && now < networkCloseTimeMS)) || ((networkCloseTimeMS < networkOpenTimeMS) && (now > networkOpenTimeMS || now < networkCloseTimeMS)))
    {
      // The network is open.
      halToggleLed(BOARDLED0);
    }
    else
    {
      // The network is closed.
      halSetLed(BOARDLED0);
      networkOpenTimeMS = networkCloseTimeMS = 0;
    }
    emberEventControlSetDelayMS(ledEventControl, LED_BLINK_PERIOD_MS << 1);
  }
  else
  {
    halClearLed(BOARDLED0);
  }
}

void emberAfHalButtonIsrCallback(uint8_t button,
                                 uint8_t state)
{
  if (state == BUTTON_RELEASED)
  {
    mostRecentButton = button;
    //    halToggleLed(BOARDLED0);
    //    emberAfCorePrintln("Button");
    emberEventControlSetActive(commissioningEventControl);
  }
}


/** @brief StateChange
 *
 * This callback is called when a device's state changes.
 *
 * @param nodeId   Ver.: always
 * @param state   Ver.: always
 */
void emberAfPluginDeviceTableStateChangeCallback(EmberNodeId nodeId,
                                                 uint8_t state)
{
  emberAfCorePrintln("StateChangeCallback, nodeId=%d, state=%d\r", nodeId, state);
  sendResponsePacket(RSP_DEV_PROPERTY, nodeId);
  return;
}

boolean emberAfMessageSentCallback(EmberOutgoingMessageType type,
                                   int16u indexOrDestination,
                                   EmberApsFrame *apsFrame,
                                   int16u msgLen,
                                   int8u *message,
                                   EmberStatus status)
{
  //	emberAfCorePrintln("MessageSentCallback, message:%s, status=%d\r", message, status);
  emberAfPluginDeviceTableMessageSentStatus(indexOrDestination,
                                            status,
                                            apsFrame->profileId,
                                            apsFrame->clusterId);

  if (status != EMBER_SUCCESS)
  {
    emberAfAppPrintln("%2x failed with code %x", indexOrDestination, status);

    if (indexOrDestination >= EMBER_DISCOVERY_ACTIVE_NODE_ID)
    {
      return false;
    }
  }
  return false;
}

#define TOLOWER(x) ((x) | 0x20)
int convert_device_name_to_eui64(uint8_t *eui64, const char *dev_name)
{
  uint64_t result = 0;
  uint32_t value;
  int i;
  while (isxdigit(*dev_name))
  {
    value = isdigit(*dev_name) ? (*dev_name - '0') : (TOLOWER(*dev_name) - 'a' + 10);
    if (value >= 16)
    {
      emberAfCorePrintln("The device name contains non hex character\r");
      return -1;
    }
    result = result * 16 + value;
    dev_name++;
  }
  for (i = 0; i < EUI64_SIZE; i++)
  {
    eui64[i] = (uint8_t)(result >> (i * 8)) & (0xFF);
  }
  return 0;
}

void cmdHandler(void)
{
  uint8_t cmd = 0;
  uint16_t len = 0, tmp = 0, dev_idx;
  int ret = 0;
  EmberEUI64 eui64;

  emberSerialReadData(APP_COM, (uint8_t *)(&len), 2, NULL);
  emberSerialReadData(APP_COM, (uint8_t *)(&tmp), 2, NULL);

  if (len == (tmp ^ 0xFFFF))
  {
    emberSerialReadData(APP_COM, rx_buf, len, NULL);
    emberAfCorePrintln("Received Data:%s\r", rx_buf);
  }
  else
  {
    emberAfCorePrintln("Packet length error\r");
    emberSerialFlushRx(APP_COM);
    return;
  }

  cJSON *root = NULL;
  root = cJSON_Parse((char *)rx_buf);

  if (root == NULL)
  {
    emberAfCorePrintln("JSON Parse Error\r");
    cJSON_Delete(root);
    return;
  }
  char *device_name = cJSON_GetObjectItem(root, "dn")->valuestring;
  //    char *product_key = cJSON_GetObjectItem(root, "pk")->valuestring;
  cmd = cJSON_GetObjectItem(root, "cmd")->valueint;

  ret = convert_device_name_to_eui64(eui64, device_name);
  if (ret)
  {
    emberAfCorePrintln("Convert failed\r");
  }

  switch (cmd)
  {
  case CMD_ADD_DEV:
    emberAfCorePrintln("CMD ADD_DEV received \r");
    break;
  case CMD_REMOVE_DEV:
    emberAfCorePrintln("CMD REMOVE_DEV received \r");
    break;
  case CMD_SET_PROPERTY:
    emberAfCorePrintln("CMD SET_PROPERTY received \r");

    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    int light_sw = cJSON_GetObjectItem(payload, "LightSwitch")->valueint;
    emberAfCorePrintln("LightSwitch:%d\r", light_sw);

    if (light_sw)
    {
      emberAfFillCommandOnOffClusterOn();
    }
    else
    {
      emberAfFillCommandOnOffClusterOff();
    }
    dev_idx = emberAfDeviceTableGetFirstIndexFromEui64(eui64);
    if (dev_idx != EMBER_AF_PLUGIN_DEVICE_TABLE_NULL_INDEX)
    {
      emberAfCorePrintln("Device Index=%d\r", dev_idx);
      emberAfDeviceTableCommandIndexSend(dev_idx);
    }
    else
    {
      emberAfCorePrintln("Failed to find Device Index=%d\r", dev_idx);
    }
    break;
  case CMD_GET_PROPERTY:
    emberAfCorePrintln("CMD GET_PROPERTY received \r");
    uint16_t node_id = emberAfDeviceTableGetNodeIdFromEui64(eui64);
    sendResponsePacket(RSP_DEV_PROPERTY, node_id);
    break;
  default:
    emberAfCorePrintln("Unkown CMD received \r\n");
    break;
  }
  cJSON_Delete(root);
}

void emberAfMainTickCallback(void)
{
  uint16_t numSerial = 0;

  numSerial = emberSerialReadAvailable(APP_COM);

  if (numSerial < 4)
    return;
  cmdHandler();
}

/** @brief Main Init
 *
 * This function is called from the application's main function. It gives the
 * application a chance to do any initialization required at system startup. Any
 * code that you would normally put into the top of the application's main()
 * routine should be put into this function. This is called before the clusters,
 * plugins, and the network are initialized so some functionality is not yet
 * available.
        Note: No callback in the Application Framework is
 * associated with resource cleanup. If you are implementing your application on
 * a Unix host where resource cleanup is a consideration, we expect that you
 * will use the standard Posix system calls, including the use of atexit() and
 * handlers for signals such as SIGTERM, SIGINT, SIGCHLD, SIGPIPE and so on. If
 * you use the signal() function to register your signal handler, please mind
 * the returned value which may be an Application Framework function. If the
 * return value is non-null, please make sure that you call the returned
 * function from your handler to avoid negating the resource cleanup of the
 * Application Framework itself.
 *
 */
void emberAfMainInitCallback(void)
{
  halInternalInitLed();
  emberSerialInit(APP_COM, 115200, PARITY_NONE, 1);
  emberEventControlSetDelayMS(subdevStateEventControl, SUBDEV_STATE_PERIOD_MS);
}
