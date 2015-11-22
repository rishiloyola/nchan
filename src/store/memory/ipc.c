//worker processes of the world, unite.
#include <nchan_module.h>
#include <ngx_channel.h>
#include <assert.h>
#include "ipc.h"
#include "shmem.h"
#include "store-private.h"

#define DEBUG_LEVEL NGX_LOG_DEBUG
//#define DEBUG_LEVEL NGX_LOG_WARN

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "IPC(%i):" fmt, memstore_slot(), ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "IPC(%i):" fmt, memstore_slot(), ##args)



static void ipc_read_handler(ngx_event_t *ev);

ipc_t *ipc_create(ngx_cycle_t *cycle) {
  ipc_t *ipc;
  ipc = ngx_calloc(sizeof(*ipc), cycle->log);
  if(ipc == NULL) {
    return NULL;
  }
  DBG("created IPC %p", ipc);
  return ipc;
}

ngx_int_t ipc_destroy(ipc_t *ipc, ngx_cycle_t *cycle) {
  DBG("destroying IPC %p", ipc);
  ngx_free(ipc);
  return NGX_OK;
}

ngx_int_t ipc_set_handler(ipc_t *ipc, void (*alert_handler)(ngx_int_t, ngx_uint_t, void *)) {
  ipc->handler=alert_handler;
  return NGX_OK;
}

static void ipc_try_close_fd(ngx_socket_t *fd) {
  if(*fd != NGX_INVALID_FILE) {
    ngx_close_socket(*fd);
    *fd=NGX_INVALID_FILE;
  }
}

ngx_int_t ipc_open(ipc_t *ipc, ngx_cycle_t *cycle, ngx_int_t workers) {
//initialize pipes for workers in advance.
  static int invalid_sockets_initialized = 0;
  int                             i, j, s = 0;//, on = 1;
  ngx_int_t                       last_expected_process = ngx_last_process;
  ipc_process_t                  *proc;
  ngx_socket_t                   *socks;
  
  if(!invalid_sockets_initialized) {
    for(i=0; i< NGX_MAX_PROCESSES; i++) {
      proc = &ipc->process[i];
      proc->ipc = ipc;
      proc->pipe[0]=NGX_INVALID_FILE;
      proc->pipe[1]=NGX_INVALID_FILE;
      proc->c=NULL;
      proc->active = 0;
    }
    invalid_sockets_initialized=1;
  }
  
  /* here's the deal: we have no control over fork()ing, nginx's internal 
    * socketpairs are unusable for our purposes (as of nginx 0.8 -- check the 
    * code to see why), and the module initialization callbacks occur before
    * any workers are spawned. Rather than futzing around with existing 
    * socketpairs, we make our own pipes array. 
    * Trouble is, ngx_spawn_process() creates them one-by-one, and we need to 
    * do it all at once. So we must guess all the workers' ngx_process_slots in 
    * advance. Meaning the spawning logic must be copied to the T.
    * ... with some allowances for already-opened sockets...
    */
  for(i=0; i < workers; i++) {
    //copypasta from os/unix/ngx_process.c (ngx_spawn_process)
    while (s < last_expected_process && ngx_processes[s].pid != -1) {
      //find empty existing slot
      s++;
    }
    
    proc = &ipc->process[i];
    
    if(!proc->active) {
      socks = proc->pipe;
      
      assert(socks[0] == NGX_INVALID_FILE && socks[1] == NGX_INVALID_FILE);
      
      //make-a-pipe
      if (pipe(socks) == -1) {
        ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, "pipe() failed while initializing nchan IPC");
        return NGX_ERROR;
      }
      //make noth ends nonblocking
      for(j=0; j <= 1; j++) {
        if (ngx_nonblocking(socks[j]) == -1) {
          ngx_log_error(NGX_LOG_ALERT, cycle->log, ngx_errno, ngx_nonblocking_n " failed on pipe socket %i while initializing nchan", j);
          ipc_try_close_fd(&socks[0]);
          ipc_try_close_fd(&socks[1]);
          return NGX_ERROR;
        }
      }
      
      //It's ALIIIIIVE! ... erm.. active...
      proc->active = 1;
    }
    s++; //NEXT!!
  }
  return NGX_OK;
}



ngx_int_t ipc_close(ipc_t *ipc, ngx_cycle_t *cycle) {
  int i;
  ipc_process_t  *proc;
  for (i=0; i<NGX_MAX_PROCESSES; i++) {
    proc = &ipc->process[i];
    if(!proc->active) continue;
    
    assert(proc->c);
    ngx_close_connection(proc->c);

    ipc_try_close_fd(&proc->pipe[0]);
    ipc_try_close_fd(&proc->pipe[1]);
    ipc->process[i].active = 0;
  }
  return NGX_OK;
}

static ngx_int_t ipc_write_alert_fd(ngx_socket_t fd, ipc_alert_t *alert) {
  int         n;
  ngx_int_t   err;
  
  n = write(fd, alert, sizeof(*alert));
 
  if (n == -1) {
    err = ngx_errno;
    if (err == NGX_EAGAIN) {
      return NGX_AGAIN;
    }
    
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, err, "write() failed");
    assert(0);
    return NGX_ERROR;
  }
  return NGX_OK;
}

static void ipc_write_handler(ngx_event_t *ev) {
  ngx_connection_t        *c = ev->data;
  ipc_process_t           *proc = (ipc_process_t *) c->data;
  
  ipc_writev_data_t      **wevd_first = &proc->wevd_first;
  
  ipc_writev_data_t       *cur, *next;
  ngx_socket_t             fd = c->fd;
  
  for(cur = *wevd_first; cur != NULL; cur = next) {
    next = cur->next;
    if(ipc_write_alert_fd(fd, &cur->alert) == NGX_OK) {
      ngx_free(cur);
    }
    else {
      break;
    }
  }
  
  *wevd_first = cur;
  if(cur == NULL) {
    proc->wevd_last = NULL;
  }
  else {
    //need to write some more
    DBG("NOT FINISHED WRITING!!");
    ngx_handle_write_event(c->write, 0);
    //ngx_add_event(c->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT);
  }
}

ngx_int_t ipc_start(ipc_t *ipc, ngx_cycle_t *cycle) {
  int                    i;    
  ngx_connection_t      *c;
  ipc_process_t         *proc;
  
  for(i=0; i< NGX_MAX_PROCESSES; i++) {
    
    proc = &ipc->process[i];
    
    if(!proc->active) continue;
    
    assert(proc->pipe[0] != NGX_INVALID_FILE);
    assert(proc->pipe[1] != NGX_INVALID_FILE);
    
    if(i==ngx_process_slot) {
      //set up read connection
      c = ngx_get_connection(proc->pipe[0], cycle->log);
      c->data = ipc;
      
      c->read->handler = ipc_read_handler;
      c->read->log = cycle->log;
      c->write->handler = NULL;
      
      ngx_add_event(c->read, NGX_READ_EVENT, 0);
      proc->c=c;
    }
    else {
      //set up write connection
      c = ngx_get_connection(proc->pipe[1], cycle->log);
      
      c->data = proc;
      
      c->read->handler = NULL;
      c->write->log = cycle->log;
      c->write->handler = ipc_write_handler;
      
      proc->c=c;
    }
  }
  return NGX_OK;
}

static ngx_int_t ipc_read_socket(ngx_socket_t s, ipc_alert_t *alert, ngx_log_t *log) {
  DBG("IPC read channel");
  ssize_t             n;
  ngx_err_t           err;
  //static char         buf[sizeof(ipc_alert_t) * 2];
  //static char        *cur;
  
  n = read(s, alert, sizeof(ipc_alert_t));
 
  if (n == -1) {
    err = ngx_errno;
    if (err == NGX_EAGAIN) {
      return NGX_AGAIN;
    }
    
    ngx_log_error(NGX_LOG_ALERT, log, err, "read() failed");
    return NGX_ERROR;
  }
 
  if (n == 0) {
    ngx_log_debug0(NGX_LOG_DEBUG_CORE, log, 0, "read() returned zero");
    return NGX_ERROR;
  }
 
  if ((size_t) n < sizeof(*alert)) {
    ngx_log_error(NGX_LOG_ALERT, log, 0, "read() returned not enough data: %z", n);
    return NGX_ERROR;
  }
  
  return n;
}

static void ipc_read_handler(ngx_event_t *ev) {
  DBG("IPC channel handler");
  //copypasta from os/unix/ngx_process_cycle.c (ngx_channel_handler)
  ngx_int_t          n;
  ipc_alert_t        alert = {0};
  ngx_connection_t  *c;
  if (ev->timedout) {
    ev->timedout = 0;
    return;
  }
  c = ev->data;
  
  while(1) {
    n = ipc_read_socket(c->fd, &alert, ev->log);
    if (n == NGX_ERROR) {
      if (ngx_event_flags & NGX_USE_EPOLL_EVENT) {
        ngx_del_conn(c, 0);
      }
      ngx_close_connection(c);
      return;
    }
    if (n == NGX_AGAIN) {
      return;
    }
    //ngx_log_debug1(NGX_LOG_DEBUG_CORE, ev->log, 0, "nchan: channel command: %d", ch.command);
    
    assert(n == sizeof(alert));
    
    if(ngx_process_slot != alert.dst_slot) {
      ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "process %i got alert intented for pid %i. don';t care, doing it anyway.", ngx_process_slot, alert.dst_slot);
    }
    ((ipc_t *)c->data)->handler(alert.src_slot, alert.code, alert.data);

  }
}

ngx_int_t ipc_alert(ipc_t *ipc, ngx_int_t slot, ngx_uint_t code, void *data, size_t data_size) {
  DBG("IPC send alert code %i to slot %i", code, slot);
  //ripped from ngx_send_channel
  
  ipc_alert_t         alert = {0};
  
  alert.src_slot = memstore_slot();
  alert.dst_slot = slot;
  alert.code = code;
  assert(data_size < IPC_DATA_SIZE * sizeof(void *));
  
  ngx_memcpy(alert.data, data, data_size);
  
  assert(alert.src_slot != alert.dst_slot);
  
#if (FAKESHARD)
  
  //switch to destination
  memstore_fakeprocess_push(alert.dst_slot);
  ipc->handler(alert.src_slot, alert.code, alert.data);
  memstore_fakeprocess_pop();
  //switch back  
  
#else
  
  
  ipc_process_t      *proc = &ipc->process[slot];
  ipc_writev_data_t  *wd = ngx_alloc(sizeof(*wd), ngx_cycle->log);
  wd->alert = alert;
  if(proc->wevd_last) {
    proc->wevd_last->next = wd;
  }
  wd->next = NULL;
  proc->wevd_last = wd;
  if(! proc->wevd_first) {
    proc->wevd_first = wd;
  }
  
  ipc_write_handler(proc->c->write);
  
  //ngx_handle_write_event(ipc->c[slot]->write, 0);
  //ngx_add_event(ipc->c[slot]->write, NGX_WRITE_EVENT, NGX_CLEAR_EVENT);
  
#endif

  return NGX_OK;
}

