#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "delay.h"

#include "critical_section.h"
#include "log.h"
#include "stores.pb-c.h"

#define MAX_STORE_COUNT 64
#define INVALID_STORE_ID MAX_STORE_COUNT

typedef struct Store {
  MuStoreType type;
  void *key;
  void *store;
} Store;

// Used to ensure this module is only initialized once
bool store_lib_inited = false;

static Store s_stores[MAX_STORE_COUNT];
static StoreFuncs s_func_table[MU_STORE_TYPE__END];

static pthread_mutex_t s_sig_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_init_lock = PTHREAD_MUTEX_INITIALIZER;

static MuLog s_mulog = MU_LOG__INIT;

static bool s_init_cond_complete = false;
static MuStoreUpdate *s_init_cond[MAX_STORE_COUNT];
static int s_num_init_conds;

// MuCmd callback table and function prototypes
// If you are adding a command, you must declare it below and add it to the lookup table
static void prv_handle_finish_conditions(void *context);
static MuCmdCallback s_cmd_cb_lookup[MU_CMD_TYPE__NUM_CMDS] = {
  [MU_CMD_TYPE__FINISH_INIT_CONDS] = prv_handle_finish_conditions,
};

static void prv_handle_finish_conditions(void *context) {
  s_init_cond_complete = true;
  pthread_mutex_unlock(&s_init_lock);
}

// signal handler for catching parent
static void prv_sigusr(int signo) {
  pthread_mutex_unlock(&s_sig_lock);
}

Store *prv_get_first_empty() {
  for (uint16_t i = 0; i < MAX_STORE_COUNT; i++) {
    if (s_stores[i].key == NULL && s_stores[i].store == NULL) {
      return &s_stores[i];
    }
  }
  return NULL;
}

static void prv_handle_store_update(uint8_t *buf, int64_t len) {
  MuStoreUpdate *update = mu_store_update__unpack(NULL, (size_t)len, buf);
  // Handle command with respective function in lookup table
  if (update->type == MU_STORE_TYPE__CMD) {
    MuCmd *cmd = mu_cmd__unpack(NULL, (size_t)update->msg.len, update->msg.data);
    if (cmd->cmd < MU_CMD_TYPE__NUM_CMDS && s_cmd_cb_lookup[cmd->cmd] != NULL) {
      s_cmd_cb_lookup[cmd->cmd](NULL);
      mu_cmd__free_unpacked(cmd, NULL);
    } else {
      LOG_DEBUG("INVALID COMMAND SENT!\n");
    }
  } else {
    if (s_init_cond_complete) {  // Default activity, call update store for type
      if (s_func_table[update->type].update_store) {
        s_func_table[update->type].update_store(update->msg, update->mask, (void *)update->key);
      }
      mu_store_update__free_unpacked(update, NULL);
    } else {  // Store initial conditions to be used in store_register
      if (s_num_init_conds < MAX_STORE_COUNT) {
        s_init_cond[s_num_init_conds] = update;
        s_num_init_conds++;
      } else {
        LOG_WARN("MU INITIAL CONDITIONS OVERFLOW!\n");
      }
    }
  }
}

// handles getting an update from python, runs as thread
static void *prv_poll_update(void *arg) {
  pid_t ppid = getppid();
  // read protos from stdin
  // compare using second proto as 'mask'
  struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
  while (true) {
    int res = poll(&pfd, 1, -1);
    if (res == -1) {
      // interrupted
      continue;
    } else if (res == 0) {
      continue;  // nothing to read
    } else {
      if (pfd.revents & POLLIN) {
        static uint8_t buf[MAX_STORE_SIZE_BYTES];
        ssize_t len = read(STDIN_FILENO, buf, sizeof(buf));
        if (len == -1) {
          LOG_DEBUG("read error while polling\n");
        }
        prv_handle_store_update(buf, len);
        // Signal parent process after poll thread created
        kill(ppid, SIGUSR2);
      } else {
        LOG_DEBUG("pollhup\n");
      }
    }
  }
  return NULL;
}

void store_config(void) {
  // do nothing after the first call
  if (store_lib_inited) {
    return;
  }

  // set up signal handler
  signal(SIGUSR1, prv_sigusr);

  // set up store pool
  memset(&s_stores, 0, sizeof(s_stores));

  // set up functions for logging
  StoreFuncs log_funcs = {
    (GetPackedSizeFunc)mu_log__get_packed_size,
    (PackFunc)mu_log__pack,
    (UnpackFunc)NULL,
    (FreeUnpackedFunc)NULL,
    (UpdateStoreFunc)NULL,
  };
  store_register(MU_STORE_TYPE__LOG, log_funcs, &s_mulog, NULL);
  // set up polling thread
  pthread_t poll_thread;
  pthread_create(&poll_thread, NULL, prv_poll_update, NULL);

  // Lock for initial conditions, continue when FINISH_INIT_CONDS recv'd
  pthread_mutex_lock(&s_init_lock);
  pthread_mutex_lock(&s_init_lock);
  pthread_mutex_unlock(&s_init_lock);

  store_lib_inited = true;
}

void store_register(MuStoreType type, StoreFuncs funcs, void *store, void *key) {
  if (store == NULL) {
    LOG_DEBUG("invalid store\n");
    return;
  }
  if (store_get(type, key)) {
    return;
  }

  s_func_table[type] = funcs;
  // malloc a proto as a store and return a pointer to it
  Store *local_store = prv_get_first_empty();
  local_store->type = type;
  local_store->store = store;
  local_store->key = key;

  // Need to check each index, as freed and set to NULL once used
  for (uint16_t i = 0; i < MAX_STORE_COUNT; i++) {
    if (s_init_cond[i] != NULL) {
      if (s_init_cond[i]->type == type && (void *)(intptr_t)s_init_cond[i]->key == key) {
        funcs.update_store(s_init_cond[i]->msg, s_init_cond[i]->mask, key);
        mu_store_update__free_unpacked(s_init_cond[i], NULL);
        s_init_cond[i] = NULL;
      }
    }
  }
}

void *store_get(MuStoreType type, void *key) {
  // just linear search for it
  // if key is NULL just get first one with right type
  for (uint16_t i = 0; i < MAX_STORE_COUNT; i++) {
    if (s_stores[i].type == type && (key == NULL || s_stores[i].key == key)) {
      return s_stores[i].store;
    }
  }
  return NULL;
}

void store_export(MuStoreType type, void *store, void *key) {
  // Serialize store to proto
  size_t packed_size = s_func_table[type].get_packed_size(store);
  uint8_t *store_buf = malloc(packed_size);
  s_func_table[type].pack(store, store_buf);

  // Set up message sent to python
  MuStoreInfo msg = MU_STORE_INFO__INIT;
  msg.key = (uint64_t)key;
  msg.type = type;
  msg.msg.data = store_buf;
  msg.msg.len = packed_size;

  // Serialize export message
  size_t export_size = mu_store_info__get_packed_size(&msg);
  uint8_t *export_buf = malloc(export_size);
  mu_store_info__pack(&msg, export_buf);

  pthread_mutex_lock(&s_sig_lock);
  // write proto to fifo
  ssize_t written = write(STDOUT_FILENO, export_buf, export_size);
  // wait for signal that parent got message
  pthread_mutex_lock(&s_sig_lock);
  pthread_mutex_unlock(&s_sig_lock);

  if (written == -1) {
    LOG_DEBUG("write error while exporting\n");
  }

  // free memory
  free(export_buf);
  free(store_buf);
}

void log_export(char *buf, uint16_t len) {
  store_config();
  s_mulog.log.len = len;
  s_mulog.log.data = (uint8_t *)buf;
  store_export(MU_STORE_TYPE__LOG, &s_mulog, NULL);
}
