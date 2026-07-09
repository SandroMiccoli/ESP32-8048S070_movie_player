#pragma once

// Tiny helpers shared by the MQTT callback and the playback loop.
// Keep the MQTT callback free of SD / display work — only queue commands.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class PlayCommand : uint8_t
{
  Idle = 0,
  Alert = 1
};

struct MqttCommandState
{
  QueueHandle_t queue = nullptr;
  volatile bool abortPlayback = false;
  volatile bool playingAlert = false;
};

inline bool mqttCommandInit(MqttCommandState &state, UBaseType_t depth = 4)
{
  if (state.queue != nullptr)
  {
    return true;
  }
  state.queue = xQueueCreate(depth, sizeof(PlayCommand));
  state.abortPlayback = false;
  return state.queue != nullptr;
}

inline bool mqttCommandSend(MqttCommandState &state, PlayCommand cmd)
{
  if (state.queue == nullptr)
  {
    return false;
  }
  // Only abort mid-play when interrupting idle — do not cut an alert short.
  if (cmd == PlayCommand::Alert && !state.playingAlert)
  {
    state.abortPlayback = true;
  }
  // Latest command wins if the queue is full.
  if (xQueueSend(state.queue, &cmd, 0) != pdTRUE)
  {
    PlayCommand discarded;
    xQueueReceive(state.queue, &discarded, 0);
    return xQueueSend(state.queue, &cmd, 0) == pdTRUE;
  }
  return true;
}

inline bool mqttCommandReceive(MqttCommandState &state, PlayCommand &out, TickType_t ticks = 0)
{
  if (state.queue == nullptr)
  {
    return false;
  }
  return xQueueReceive(state.queue, &out, ticks) == pdTRUE;
}

inline void mqttCommandClearAbort(MqttCommandState &state)
{
  state.abortPlayback = false;
}

inline bool mqttCommandShouldAbort(const MqttCommandState &state)
{
  return state.abortPlayback;
}
