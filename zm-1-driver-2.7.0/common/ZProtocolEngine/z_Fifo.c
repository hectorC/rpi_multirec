



#include "z_Fifo.h"


static int  z_FifoLock      (zFifo* a_Fifo);
static int  z_FifoUnlock    (zFifo* a_Fifo);




zFifo* z_FifoCreate(int a_Size)
{
    zFifo* fifo = NULL;

#if (LINUX_KERNEL_MODULE == 1)  

    
    if(in_interrupt()) {
        fifo = (zFifo*)kzalloc(sizeof(zFifo), GFP_ATOMIC);
    }
    else {
        fifo = (zFifo*)kzalloc(sizeof(zFifo), GFP_KERNEL);
    }

    if(fifo == NULL) {
        return NULL;
    }


    
    if(in_interrupt()) {
        fifo->buffer = (void**)kzalloc(a_Size * sizeof(void*), GFP_ATOMIC);
    }
    else {
        fifo->buffer = (void**)kzalloc(a_Size * sizeof(void*), GFP_KERNEL);
    }

    if(fifo->buffer == NULL) {
        kfree(fifo);
        return NULL;
    }

    
    spin_lock_init(&fifo->lock);

#elif (MACOS_KERNEL_MODULE == 1)

    
    fifo = (zFifo*)IOMalloc(sizeof(zFifo));
    if(fifo == NULL) {
        return NULL;
    }

    memset(fifo, 0, sizeof(zFifo));

    
    fifo->buffer = (void**)IOMalloc(a_Size * sizeof(void*));
    if(fifo->buffer == NULL) {
        IOFree(fifo, sizeof(zFifo));
        return NULL;
    }

    memset(fifo->buffer, 0, a_Size * sizeof(void*));

    
    fifo->lock = IOSimpleLockAlloc();

    if(fifo->lock == NULL) {
        IOFree((void*)fifo->buffer, a_Size * sizeof(void*));
        IOFree(fifo, sizeof(zFifo));
        return NULL;
    }

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    fifo = (zFifo*)ExAllocatePoolWithTag(NonPagedPool, sizeof(zFifo), Z_FIFO_POOL_TAG);
    if(fifo == NULL) {
        return NULL;
    }

    RtlZeroMemory((void*)fifo, sizeof(zFifo));

    
    fifo->buffer = (void**)ExAllocatePoolWithTag(NonPagedPool, a_Size * sizeof(void*), Z_FIFO_POOL_TAG);
    if(fifo->buffer == NULL) {
        ExFreePoolWithTag((void*)fifo, Z_FIFO_POOL_TAG);
        return NULL;
    }

    
    KeInitializeSpinLock(&fifo->lock);

#elif (WINDOWS_USER_MODULE == 1)

    
    fifo = (zFifo*)calloc(1, sizeof(zFifo));

    if(fifo == NULL) {
        return NULL;
    }

    
    fifo->buffer = (void**)calloc(1, a_Size * sizeof(void*));

    if(fifo->buffer == NULL) {
        free(fifo);
        return NULL;
    }

    
    fifo->mutex = CreateMutex(NULL, FALSE, NULL);

#elif (MACOS_USER_MODULE == 1)
    
    fifo = IONewZero(zFifo, 1);
    if (fifo == NULL) return NULL;
    
    fifo->buffer = (void**)IOMallocZero(a_Size * sizeof(void*));
    
    
#else   

    
    fifo = (zFifo*)calloc(1, sizeof(zFifo));

    if(fifo == NULL) {
        return NULL;
    }

    
    fifo->buffer = (void**)calloc(1, a_Size * sizeof(void*));

    if(fifo->buffer == NULL) {
        free(fifo);
        return NULL;
    }

#endif

    
    fifo->size         = a_Size;
    fifo->remaining    = a_Size;
    
    return fifo;
}


zFifo* z_FifoDelete(zFifo* a_Fifo)
{
    if(a_Fifo == NULL) {
        return NULL;
    }

#if (LINUX_KERNEL_MODULE == 1) 

    if(a_Fifo->buffer != NULL) {
        kfree(a_Fifo->buffer);
    }

    kfree(a_Fifo);

#elif (MACOS_KERNEL_MODULE == 1)

    
    if (a_Fifo->lock != NULL) {
        IOSimpleLockFree(a_Fifo->lock);
    }

    
    if(a_Fifo->buffer != NULL) {
        IOFree((void*)a_Fifo->buffer, a_Fifo->size * sizeof(void*));
    }

    IOFree((void*)a_Fifo, sizeof(zFifo));

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    if (a_Fifo->buffer != NULL) {
        ExFreePoolWithTag((void*)a_Fifo->buffer, Z_FIFO_POOL_TAG);
    }

    ExFreePoolWithTag((void*)a_Fifo, Z_FIFO_POOL_TAG);

#elif (WINDOWS_USER_MODULE == 1)

    
    CloseHandle(a_Fifo->mutex);

    
    if(a_Fifo->buffer != NULL) {
        free(a_Fifo->buffer);
    }

    
    free(a_Fifo);

#elif (MACOS_USER_MODULE == 1)
    
    IOFree(a_Fifo->buffer, a_Fifo->size * sizeof(void*));
    
    IODelete(a_Fifo, zFifo, 1);
    
#else 

    if(a_Fifo->buffer != NULL) {
        free(a_Fifo->buffer);
    }

    free(a_Fifo);

#endif

    return NULL;
}




static int z_FifoLock(zFifo* a_Fifo) {

#if (LINUX_KERNEL_MODULE == 1)

    
    spin_lock_irqsave(&a_Fifo->lock, a_Fifo->lockFlags);

#elif (MACOS_KERNEL_MODULE == 1)

    
    IOSimpleLockLock(a_Fifo->lock);

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    KeAcquireSpinLock(&a_Fifo->lock, &a_Fifo->lockFlags);

#elif (WINDOWS_USER_MODULE == 1)

    
    WaitForSingleObject(a_Fifo->mutex, INFINITE);

#else

    

#endif

    return 0;
}


static int z_FifoUnlock(zFifo* a_Fifo) {

#if (LINUX_KERNEL_MODULE == 1)

    
    spin_unlock_irqrestore(&a_Fifo->lock, a_Fifo->lockFlags);

#elif (MACOS_KERNEL_MODULE == 1)

    
    IOSimpleLockUnlock(a_Fifo->lock);

#elif (WINDOWS_KERNEL_MODULE == 1)

    
    KeReleaseSpinLock(&a_Fifo->lock, a_Fifo->lockFlags);

#elif (WINDOWS_USER_MODULE == 1)

    
    ReleaseMutex(a_Fifo->mutex);

#else

    

#endif

    return 0;
}




int z_FifoPush(zFifo* a_Fifo, void* a_Data)
{
    if(a_Fifo == NULL) {
        return -1;
    }

    z_FifoLock(a_Fifo);

    
    if(a_Fifo->remaining == 0) {
        z_FifoUnlock(a_Fifo);
        return 1;
    }

    
    a_Fifo->buffer[a_Fifo->wrPtr++] = a_Data;

    if(a_Fifo->wrPtr >= a_Fifo->size) {
        a_Fifo->wrPtr -= a_Fifo->size;
    }

    a_Fifo->occupancy++;
    a_Fifo->remaining--;

    z_FifoUnlock(a_Fifo);

    return 0;
}


int z_FifoPop(zFifo* a_Fifo, void** a_Data)
{
    if(a_Fifo == NULL || a_Data == NULL) {
        return -1;
    }

    z_FifoLock(a_Fifo);

    
    if(a_Fifo->occupancy == 0) {
        z_FifoUnlock(a_Fifo);
        return 1;
    }

    
    (*a_Data) = a_Fifo->buffer[a_Fifo->rdPtr];
    a_Fifo->buffer[a_Fifo->rdPtr++] = NULL;

    if(a_Fifo->rdPtr >= a_Fifo->size) {
        a_Fifo->rdPtr -= a_Fifo->size;
    }

    a_Fifo->occupancy--;
    a_Fifo->remaining++;

    z_FifoUnlock(a_Fifo);

    return 0;
}


int z_FifoClear(zFifo* a_Fifo)
{
    size_t  i;

    if(a_Fifo == NULL) {
        return -1;
    }

    z_FifoLock(a_Fifo);

    for(i=0; i<(unsigned int)a_Fifo->size; ++i)
        a_Fifo->buffer[i] = NULL;

    a_Fifo->rdPtr       = 0;
    a_Fifo->wrPtr       = 0;

    a_Fifo->occupancy   = 0;
    a_Fifo->remaining   = a_Fifo->size;

    z_FifoUnlock(a_Fifo);

    return 0;
}
