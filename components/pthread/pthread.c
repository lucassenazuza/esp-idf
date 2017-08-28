#include <errno.h>
#include <pthread.h>
#include <string.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/list.h"

#define LOG_LOCAL_LEVEL CONFIG_LOG_DEFAULT_LEVEL//ESP_LOG_VERBOSE
#include "esp_log.h"
const static char *TAG = "esp_pthread";

#define ESP_PTHREAD_LOGE( _tag_, format, ... )  ESP_LOGE(_tag_, format, ##__VA_ARGS__)
#define ESP_PTHREAD_LOGW( _tag_, format, ... )  ESP_LOGW(_tag_, format, ##__VA_ARGS__)
#define ESP_PTHREAD_LOGI( _tag_, format, ... )  ESP_LOGI(_tag_, format, ##__VA_ARGS__)
#define ESP_PTHREAD_LOGD( _tag_, format, ... )  ESP_LOGD(_tag_, format, ##__VA_ARGS__)
#define ESP_PTHREAD_LOGV( _tag_, format, ... )  ESP_LOGV(_tag_, format, ##__VA_ARGS__)

#define PTHREAD_TASK_STATE_RUN		0
#define PTHREAD_TASK_STATE_EXIT		1

typedef struct {
	ListItem_t		list_item;
	TaskHandle_t	join_task;
	int				state;
	bool			detached;
} esp_pthread_t;

typedef struct {
	void *(*func)(void *);
	void *arg;
} esp_pthread_task_arg_t;

typedef struct {
	ListItem_t			list_item;
	SemaphoreHandle_t 	sem;
	int 				type;
} esp_pthread_mutex_t;


static SemaphoreHandle_t s_once_mux = NULL;
static SemaphoreHandle_t s_threads_mux = NULL;

static List_t s_threads_list;


static int IRAM_ATTR pthread_mutex_lock_internal(esp_pthread_mutex_t *mux, TickType_t tmo);

int esp_pthread_init(void)
{
	vListInitialise((List_t *)&s_threads_list);
	s_once_mux = xSemaphoreCreateMutex();
	if (s_once_mux == NULL)
		return ESP_FAIL;
	s_threads_mux = xSemaphoreCreateMutex();
	if (s_threads_mux == NULL) {
		vSemaphoreDelete(s_once_mux);
		return ESP_FAIL;
	}
	return ESP_OK;
}

static TaskHandle_t pthread_find_handle(pthread_t thread)
{
	ListItem_t const *list_end = listGET_END_MARKER(&s_threads_list);
	ListItem_t *list_item = listGET_HEAD_ENTRY(&s_threads_list);
	while (list_item != list_end) {
		esp_pthread_t *pthread = listGET_LIST_ITEM_OWNER(list_item);
    	if ((pthread_t)pthread == thread) {
    		return (TaskHandle_t)listGET_LIST_ITEM_VALUE(list_item);
    	}
		list_item = listGET_NEXT(list_item);
	}
	return NULL;
}

static esp_pthread_t *pthread_find(TaskHandle_t task_handle)
{
	ListItem_t const *list_end = listGET_END_MARKER(&s_threads_list);
	ListItem_t *list_item = listGET_HEAD_ENTRY(&s_threads_list);
	while (list_item != list_end) {
		TaskHandle_t cur_handle = (TaskHandle_t)listGET_LIST_ITEM_VALUE(list_item);
    	if (task_handle == cur_handle) {
    		return (esp_pthread_t *)listGET_LIST_ITEM_OWNER(list_item);
    	}
		list_item = listGET_NEXT(list_item);
	}
	return NULL;
}

static void pthread_delete(esp_pthread_t *pthread)
{
	uxListRemove(&pthread->list_item);
	free(pthread);
}

static void pthread_task_func(void *arg)
{
	esp_pthread_task_arg_t *task_arg = (esp_pthread_task_arg_t *)arg;

	ESP_PTHREAD_LOGV(TAG, "%s ENTER %p", __FUNCTION__, task_arg->func);

	// wait for start
	xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

	ESP_PTHREAD_LOGV(TAG, "%s START %p", __FUNCTION__, task_arg->func);
	task_arg->func(task_arg->arg);
	ESP_PTHREAD_LOGV(TAG, "%s END %p", __FUNCTION__, task_arg->func);
	free(task_arg);

    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
		assert(false && "Failed to lock threads list!");
    }
	esp_pthread_t *pthread = pthread_find(xTaskGetCurrentTaskHandle());
	if (!pthread) {
		assert(false && "Failed to find pthread for current task!");
    }
	if (pthread->detached) {
		// auto-free for detached threads
		pthread_delete(pthread);
	} else {
		// Remove from list, it indicates that task has exited
		if (pthread->join_task) {
			// notify join
			xTaskNotify(pthread->join_task, 0, eNoAction);
		} else {
			pthread->state = PTHREAD_TASK_STATE_EXIT;
		}
	}
    xSemaphoreGive(s_threads_mux);

	vTaskDelete(NULL);

	ESP_PTHREAD_LOGV(TAG, "%s EXIT", __FUNCTION__);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine) (void *), void *arg)
{
	TaskHandle_t xHandle = NULL;

	ESP_PTHREAD_LOGV(TAG, "%s", __FUNCTION__);
	if (attr) {
		ESP_PTHREAD_LOGE(TAG, "Attrs not supported!");
		return EINVAL;
	}
	esp_pthread_task_arg_t *task_arg = malloc(sizeof(esp_pthread_task_arg_t));
	if (task_arg == NULL) {
		ESP_PTHREAD_LOGE(TAG, "Failed to allocate task args!");
		errno = ENOMEM;
		return ENOMEM;
	}
	memset(task_arg, 0, sizeof(esp_pthread_task_arg_t));
	esp_pthread_t *pthread = malloc(sizeof(esp_pthread_t));
	if (pthread == NULL) {
		ESP_PTHREAD_LOGE(TAG, "Failed to allocate pthread data!");
		free(task_arg);
		errno = ENOMEM;
		return ENOMEM;
	}
	memset(pthread, 0, sizeof(esp_pthread_t));
	task_arg->func = start_routine;
	task_arg->arg = arg;
    BaseType_t res = xTaskCreate(&pthread_task_func, "pthread", CONFIG_ESP32_PTHREAD_TASK_STACK_SIZE_DEFAULT,
    	task_arg, CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT, &xHandle);
	if(res != pdPASS) {
		ESP_PTHREAD_LOGE(TAG, "Failed to create task!");
		free(pthread);
		free(task_arg);
		if (res == errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY) {
			errno = ENOMEM;
			return ENOMEM;
		} else {
			errno = EAGAIN;
			return EAGAIN;
		}
	}
	vListInitialiseItem((ListItem_t *)&pthread->list_item);
	listSET_LIST_ITEM_OWNER((ListItem_t *)&pthread->list_item, pthread);
	listSET_LIST_ITEM_VALUE((ListItem_t *)&pthread->list_item, (TickType_t)xHandle);

    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
		assert(false && "Failed to lock threads list!");
    }
	vListInsertEnd((List_t *)&s_threads_list, (ListItem_t *)&pthread->list_item);
    xSemaphoreGive(s_threads_mux);

	// start task
	xTaskNotify(xHandle, 0, eNoAction);

	*thread = (pthread_t)pthread; // pointer value fit into pthread_t (uint32_t)

	ESP_PTHREAD_LOGV(TAG, "Created task %x", (uint32_t)xHandle);

	return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
	esp_pthread_t *pthread = (esp_pthread_t *)thread;
	int ret = 0;

	ESP_PTHREAD_LOGV(TAG, "%s %p", __FUNCTION__, pthread);

	// find task
    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
		assert(false && "Failed to lock threads list!");
    }
    TaskHandle_t handle = pthread_find_handle(thread);
    if (!handle) {
		errno = ESRCH; // not found
		ret = ESRCH;
    } else if (pthread->join_task) {
		errno = EINVAL; // already have waiting task to join
		ret = EINVAL;
    } else if (handle == xTaskGetCurrentTaskHandle()) {
		errno = EDEADLK; // join to self not allowed
		ret = EDEADLK;
    } else {
		esp_pthread_t *cur_pthread = pthread_find(xTaskGetCurrentTaskHandle());
		if (cur_pthread && cur_pthread->join_task == handle) {
			errno = EDEADLK; // join to each other not allowed
			ret = EDEADLK;
		} else {
			if (pthread->state == PTHREAD_TASK_STATE_RUN) {
				pthread->join_task = xTaskGetCurrentTaskHandle();
			} else {
				pthread_delete(pthread);
			}
		}
    }
    xSemaphoreGive(s_threads_mux);

    if (ret == 0 && pthread->join_task) {
		xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
	    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
			assert(false && "Failed to lock threads list!");
	    }
		pthread_delete(pthread);
        xSemaphoreGive(s_threads_mux);
    }

    if (retval) {
    	*retval = 0; // no exit code in FreeRTOS
    }

	ESP_PTHREAD_LOGV(TAG, "%s %p EXIT %d", __FUNCTION__, pthread, ret);
	return ret;
}

int pthread_detach(pthread_t thread)
{
	esp_pthread_t *pthread = (esp_pthread_t *)thread;
	int ret = 0;

    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
		assert(false && "Failed to lock threads list!");
    }
    TaskHandle_t handle = pthread_find_handle(thread);
    if (!handle) {
		errno = ESRCH; // not found
		ret = ESRCH;
	} else {
		pthread->detached = true;
	}
    xSemaphoreGive(s_threads_mux);
	ESP_PTHREAD_LOGV(TAG, "%s %p EXIT %d", __FUNCTION__, pthread, ret);
	return ret;
}

int pthread_cancel(pthread_t thread)
{
	assert(false && "pthread_cancel not supported!");
	return -1;
}

int sched_yield( void )
{
	vTaskDelay(0);
	return 0;
}

pthread_t pthread_self(void)
{
    if (xSemaphoreTake(s_threads_mux, portMAX_DELAY) != pdTRUE) {
		assert(false && "Failed to lock threads list!");
    }
	esp_pthread_t *pthread = pthread_find(xTaskGetCurrentTaskHandle());
	if (!pthread) {
		assert(false && "Failed to find current thread ID!");
	}
    xSemaphoreGive(s_threads_mux);
	return (pthread_t)pthread;
}

int pthread_equal(pthread_t t1, pthread_t t2)
{
	return t1 == t2 ? 1 : 0;
}

/***************** KEY ******************/
int pthread_key_create(pthread_key_t *key, void (*destructor)(void*))
{
	static int s_created;

	//TODO: Key destructors not suppoted!
	if (s_created) {
		// key API supports just one key necessary by libstdcxx threading implementation
		return ENOMEM;
	}
	*key = 1;
	s_created = 1;
	return 0;
}

int pthread_key_delete(pthread_key_t key)
{
	assert(false && "pthread_key_delete not supported!");
	return -1;
}

void *pthread_getspecific(pthread_key_t key)
{
	assert(false && "pthread_getspecific not supported!");
	return NULL;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
	assert(false && "pthread_setspecific not supported!");
	return -1;
}

/***************** ONCE ******************/
int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
	if (once_control == NULL || init_routine == NULL || !once_control->is_initialized) {
		ESP_PTHREAD_LOGE(TAG, "%s: Invalid args!", __FUNCTION__);
		return EINVAL;
	}

	TaskHandle_t cur_task = xTaskGetCurrentTaskHandle();
	// do not take mutex if OS is not running yet
    if (!cur_task || xSemaphoreTake(s_once_mux, portMAX_DELAY) == pdTRUE)
    {
		if (!once_control->init_executed) {
			ESP_PTHREAD_LOGV(TAG, "%s: call init_routine %p", __FUNCTION__, once_control);
			init_routine();
			once_control->init_executed = 1;
		}
		if (cur_task) {
        	xSemaphoreGive(s_once_mux);
		}
    }
    else
    {
		ESP_PTHREAD_LOGE(TAG, "%s: Failed to lock!", __FUNCTION__);
		return EBUSY;
    }

	return 0;
}

/***************** MUTEX ******************/
static int mutexattr_check(const pthread_mutexattr_t *attr)
{
	if (attr->type < PTHREAD_MUTEX_NORMAL || attr->type > PTHREAD_MUTEX_RECURSIVE) {
		return EINVAL;
	}
	return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	int type = PTHREAD_MUTEX_NORMAL;

	if (!mutex) {
		errno = EINVAL;
		return EINVAL;
	}

	if (attr) {
		if (!attr->is_initialized) {
			errno = EINVAL;
			return EINVAL;
		}
		int res = mutexattr_check(attr);
		if (res) {
			errno = res;
			return res;
		}
		type = attr->type;
	}

	esp_pthread_mutex_t *mux = (esp_pthread_mutex_t *)malloc(sizeof(esp_pthread_mutex_t));
    if (!mux) {
        errno = ENOMEM;
        return ENOMEM;
	}
	mux->type = type;

	if (mux->type == PTHREAD_MUTEX_RECURSIVE) {
        mux->sem = xSemaphoreCreateRecursiveMutex();
    } else {
        mux->sem = xSemaphoreCreateMutex();
    }
    if (!mux->sem) {
        free(mux);
        errno = EAGAIN;
        return EAGAIN;
	}

	*mutex = (pthread_mutex_t)mux; // pointer value fit into pthread_mutex_t (uint32_t)

	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	esp_pthread_mutex_t *mux;

	ESP_PTHREAD_LOGV(TAG, "%s %p", __FUNCTION__, mutex);

	if (!mutex) {
		errno = EINVAL;
		return EINVAL;
	}
	mux = (esp_pthread_mutex_t *)*mutex;

    // check if mux is busy
    int res = pthread_mutex_lock_internal(mux, 0);
    if (res == EBUSY) {
        errno = EBUSY;
        return EBUSY;
    }

	vSemaphoreDelete(mux->sem);
	free(mux);

	return 0;
}

static int IRAM_ATTR pthread_mutex_lock_internal(esp_pthread_mutex_t *mux, TickType_t tmo)
{
    if (mux->type == PTHREAD_MUTEX_RECURSIVE) {
        if (xSemaphoreTakeRecursive(mux->sem, tmo) != pdTRUE) {
            errno = EBUSY;
            return EBUSY;
        }
    } else {
        if (xSemaphoreTake(mux->sem, tmo) != pdTRUE) {
            errno = EBUSY;
            return EBUSY;
        }
    }

    return 0;
}

int IRAM_ATTR pthread_mutex_lock(pthread_mutex_t *mutex)
{
	if (!mutex) {
		errno = EINVAL;
		return EINVAL;
	}
    return pthread_mutex_lock_internal((esp_pthread_mutex_t *)*mutex, portMAX_DELAY);
}

int IRAM_ATTR pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	if (!mutex) {
		errno = EINVAL;
		return EINVAL;
	}
    return pthread_mutex_lock_internal((esp_pthread_mutex_t *)*mutex, 0);
}

int IRAM_ATTR pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	esp_pthread_mutex_t *mux;

	if (!mutex) {
		errno = EINVAL;
		return EINVAL;
	}
	mux = (esp_pthread_mutex_t *)*mutex;

	if (mux->type == PTHREAD_MUTEX_RECURSIVE) {
        xSemaphoreGiveRecursive(mux->sem);
    } else {
        xSemaphoreGive(mux->sem);
	}
	return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	if (!attr) {
		errno = EINVAL;
		return EINVAL;
	}
	attr->type = PTHREAD_MUTEX_NORMAL;
	attr->is_initialized = 1;
	return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	if (!attr) {
		errno = EINVAL;
		return EINVAL;
	}
	attr->is_initialized = 0;
	return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
	assert(false && "pthread_mutexattr_gettype not supported!");
	return -1;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
	if (!attr) {
		errno = EINVAL;
		return EINVAL;
	}
	pthread_mutexattr_t tmp_attr = {.type = type};
	int res = mutexattr_check(&tmp_attr);
	if (res) {
		errno = res;
	} else {
		attr->type = type;
	}
	return res;
}

/***************** AUX ******************/
// TODO: move to newlib/time.c????
// needed for std::this_thread::sleep_for
unsigned int sleep(unsigned int seconds)
{
	usleep(seconds*1000000UL);
	return 0;
}
