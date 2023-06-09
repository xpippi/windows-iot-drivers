// Copyright (c) Microsoft Corporation.  All rights reserved.
//
// Module Name:
//
//     utils.c
//
// Abstract:
//
//    This module contains code that perform queueing and completion
//    manipulation on requests.  Also module generic functions such
//    as error logging.

#include "precomp.h"
#include "utils.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGESRP0,SerialMarkHardwareBroken)
#endif

VOID
SerialRundownIrpRefs(
    _In_ WDFREQUEST* CurrentOpRequest,
    _In_ WDFTIMER IntervalTimer,
    _In_ WDFTIMER TotalTimer,
    _In_ PSERIAL_DEVICE_EXTENSION DevExt,
    _In_ LONG RefType
    );

static const PHYSICAL_ADDRESS SerialPhysicalZero = {0};

/*++

Routine Description:

    This function is used to cancel all queued and the current irps
    for reads or for writes. Called at DPC level.

Arguments:

    QueueToClean - A pointer to the queue which we're going to clean out.

    CurrentOpRequest - Pointer to a pointer to the current request.

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialPurgeRequests(
    WDFQUEUE QueueToClean,
    WDFREQUEST* CurrentOpRequest
    )
{
    NTSTATUS status;
    PREQUEST_CONTEXT reqContext;

    WdfIoQueuePurge(QueueToClean, WDF_NO_EVENT_CALLBACK, WDF_NO_CONTEXT);

    // The queue is clean.  Now go after the current if it's there.

    if (*CurrentOpRequest) {

        PFN_WDF_REQUEST_CANCEL CancelRoutine;

        reqContext = SerialGetRequestContext(*CurrentOpRequest);
        CancelRoutine = reqContext->CancelRoutine;

        // Clear the common cancel routine but don't clear the reference because the
        // request specific cancel routine called below will clear the reference.

        status = SerialClearCancelRoutine(*CurrentOpRequest, FALSE);
        if (NT_SUCCESS(status)) {

            // Let us just call the CancelRoutine to start the next request.

            if(CancelRoutine) {

                CancelRoutine(*CurrentOpRequest);
            }
        }
    }
}

/*++

Routine Description:

    This function is used to cancel all queued and the current irps
    for reads or for writes. Called at DPC level.

Arguments:

    QueueToClean - A pointer to the queue which we're going to clean out.

    CurrentOpRequest - Pointer to a pointer to the current request.

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialFlushRequests(
    WDFQUEUE QueueToClean,
    WDFREQUEST* CurrentOpRequest
    )
{
    SerialPurgeRequests(QueueToClean,  CurrentOpRequest);

    // Since purge puts the queue state to fail requests, we have to explicitly
    // change the queue state to accept requests.

    WdfIoQueueStart(QueueToClean);

}

/*++

Routine Description:

    This function is used to make the head of the particular
    queue the current request.  It also completes the what
    was the old current request if desired.

Arguments:

    CurrentOpRequest - Pointer to a pointer to the currently active
                   request for the particular work list.  Note that
                   this item is not actually part of the list.

    QueueToProcess - The list to pull the new item off of.

    NextIrp - The next Request to process.  Note that CurrentOpRequest
              will be set to this value under protection of the
              cancel spin lock.  However, if *NextIrp is NULL when
              this routine returns, it is not necessaryly true the
              what is pointed to by CurrentOpRequest will also be NULL.
              The reason for this is that if the queue is empty
              when we hold the cancel spin lock, a new request may come
              in immediately after we release the lock.

    CompleteCurrent - If TRUE then this routine will complete the
                      request pointed to by the pointer argument
                      CurrentOpRequest.

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialGetNextRequest(
    WDFREQUEST* CurrentOpRequest,
    WDFQUEUE QueueToProcess,
    WDFREQUEST* NextRequest,
    BOOLEAN CompleteCurrent,
    PSERIAL_DEVICE_EXTENSION Extension
    )
{
    WDFREQUEST oldRequest = NULL;
    PREQUEST_CONTEXT reqContext;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Extension);

    oldRequest = *CurrentOpRequest;
    *CurrentOpRequest = NULL;

    // Check to see if there is a new request to start up.

    status = WdfIoQueueRetrieveNextRequest(QueueToProcess,
                                            CurrentOpRequest);

    if(!NT_SUCCESS(status)) {
        ASSERTMSG("WdfIoQueueRetrieveNextRequest failed",
                  status == STATUS_NO_MORE_ENTRIES);
    }

    *NextRequest = *CurrentOpRequest;

    if (CompleteCurrent) {

        if (oldRequest) {

            reqContext = SerialGetRequestContext(oldRequest);

            SerialCompleteRequest(oldRequest,
                                  reqContext->Status,
                                  reqContext->Information);
        }
    }
}

/*++

Routine Description:

    This routine attempts to remove all of the reasons there are
    references on the current read/write.  If everything can be completed
    it will complete this read/write and try to start another.

    NOTE: This routine assumes that it is called with the cancel
          spinlock held.

Arguments:

    Extension - Simply a pointer to the device extension.

    SynchRoutine - A routine that will synchronize with the isr
                   and attempt to remove the knowledge of the
                   current request from the isr.  NOTE: This pointer
                   can be null.

    IrqlForRelease - This routine is called with the cancel spinlock held.
                     This is the irql that was current when the cancel
                     spinlock was acquired.

    StatusToUse - The request's status field will be set to this value, if
                  this routine can complete the request.


Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialTryToCompleteCurrent(
    PSERIAL_DEVICE_EXTENSION Extension,
    PFN_WDF_INTERRUPT_SYNCHRONIZE  SynchRoutine OPTIONAL,
    NTSTATUS StatusToUse,
    WDFREQUEST* CurrentOpRequest,
    WDFQUEUE QueueToProcess OPTIONAL,
    WDFTIMER IntervalTimer OPTIONAL,
    WDFTIMER TotalTimer OPTIONAL,
    PSERIAL_START_ROUTINE Starter OPTIONAL,
    PSERIAL_GET_NEXT_ROUTINE GetNextRequest OPTIONAL,
    LONG RefType
    )
{
    PREQUEST_CONTEXT reqContext;

    ASSERTMSG("SerialTryToCompleteCurrent: CurrentOpRequest is NULL", *CurrentOpRequest);

     reqContext = SerialGetRequestContext(*CurrentOpRequest);

    if(RefType == SERIAL_REF_ISR || RefType == SERIAL_REF_XOFF_REF) {

        // We can decrement the reference to "remove" the fact
        // that the caller no longer will be accessing this request.

        SERIAL_CLEAR_REFERENCE(reqContext,
                                RefType);
    }

    if (SynchRoutine) {

        WdfInterruptSynchronize(Extension->WdfInterrupt,
                                SynchRoutine,
                                Extension);
    }

    // Try to run down all other references to this request.

    SerialRundownIrpRefs(CurrentOpRequest,
                        IntervalTimer,
                        TotalTimer,
                        Extension,
                        RefType);

    if(StatusToUse == STATUS_CANCELLED) {

        // This function is called from a cancelroutine. So mark
        // the request as cancelled. We need to do this because
        // we may not complete the request below if somebody
        // else has a reference to it.
        // This state variable was added to avoid calling
        // WdfRequestMarkCancelable second time on a request that
        // has cancelled but wasn't completed in the cancel routine.

        reqContext->Cancelled = TRUE;
    }

    // See if the ref count is zero after trying to complete everybody else.

    if (!SERIAL_REFERENCE_COUNT(reqContext)) {

        WDFREQUEST newRequest;

        // The ref count was zero so we should complete this
        // request.
        //
        // The following call will also cause the current request to be
        // completed.

        reqContext->Status = StatusToUse;

        if (StatusToUse == STATUS_CANCELLED) {

            reqContext->Information = 0;
        }

        if (GetNextRequest) {

            GetNextRequest(CurrentOpRequest,
                            QueueToProcess,
                            &newRequest,
                            TRUE,
                            Extension);

            if (newRequest) {

                Starter(Extension);
            }

        } else {

            WDFREQUEST oldRequest = *CurrentOpRequest;

            // There was no get next routine.  We will simply complete
            // the request.  We should make sure that we null out the
            // pointer to the pointer to this request.

            *CurrentOpRequest = NULL;

            SerialCompleteRequest(oldRequest,
                                  reqContext->Status,
                                  reqContext->Information);
        }

    } else {


    }

}

/*++

Routine Description:

     This callback is invoked for every request pending in the driver (not queue) -
     in-flight request. The Action parameter tells us why the callback is invoked -
     because the device is being stopped, removed or suspended. In this
     driver, we have told the framework not to stop or remove when there
     are pending requests, so only reason for this callback is when the system is
     suspending.

Arguments:

    Queue - Queue the request currently belongs to
    Request - Request that is currently out of queue and being processed by the driver
    Action - Reason for this callback

Return Value:

    None. Acknowledge the request so that framework can contiue suspending the
    device.

--*/
_Use_decl_annotations_
VOID
SerialEvtIoStop(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    ULONG ActionFlags
    )
{
    PREQUEST_CONTEXT reqContext;

    UNREFERENCED_PARAMETER(Queue);

    reqContext = SerialGetRequestContext(Request);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_WRITE,
                    "++SerialEvtIoStop %x %p\r\n",
                    ActionFlags,
                    Request);

    // System suspends all the timers before asking the driver to goto
    // sleep. So let us not worry about cancelling the timers. Also the
    // framework will disconnect the interrupt before calling our
    // D0Exit handler so we can be sure that nobody will touch the hardware.
    // So just acknowledge callback to say that we are okay to stop due to
    // system suspend. Please note that since we have taken a power reference
    // we will never idle out when there is an open handle. Also we have told
    // the framework to not stop for resource rebalancing or remove when there are
    // open handles, so let us not worry about that either.

    if (ActionFlags & WdfRequestStopRequestCancelable) {
        PFN_WDF_REQUEST_CANCEL cancelRoutine;

        // Request is in a cancelable state. So unmark cancelable before you
        // acknowledge. We will mark the request cancelable when we resume.

        cancelRoutine = reqContext->CancelRoutine;

        SerialClearCancelRoutine(Request, TRUE);

        // SerialClearCancelRoutine clears the cancel-routine. So set it back
        // in the context. We will need that when we resume.

        reqContext->CancelRoutine = cancelRoutine;

        reqContext->MarkCancelableOnResume = TRUE;

        ActionFlags &= ~WdfRequestStopRequestCancelable;
    }

    ASSERT(ActionFlags == WdfRequestStopActionSuspend);

    // Don't requeue the request

    WdfRequestStopAcknowledge(Request, FALSE); 

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_WRITE,
                "--SerialEvtIoStop\r\n");
}

/*++

Routine Description:

     This callback is invoked for every request pending in the driver - in-flight
     request - to notify that the hardware is ready for contiuing the processing
     of the request.

Arguments:

    Queue - Queue the request currently belongs to
    Request - Request that is currently out of queue and being processed by the driver

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialEvtIoResume(
    WDFQUEUE Queue,
    WDFREQUEST Request
    )
{
    PREQUEST_CONTEXT reqContext;

    UNREFERENCED_PARAMETER(Queue);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_WRITE,
                "++SerialEvtIoResume %p\r\n",
                Request);

    reqContext = SerialGetRequestContext(Request);

    // If we unmarked cancelable on suspend, let us mark it cancelable again.

    if (reqContext->MarkCancelableOnResume) {
        SerialSetCancelRoutine(Request, reqContext->CancelRoutine);
        reqContext->MarkCancelableOnResume = FALSE;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_WRITE, "--SerialEvtIoResume\r\n");
}

/*++

Routine Description:

    This routine runs through the various items that *could*
    have a reference to the current read/write.  It try's to remove
    the reason.  If it does succeed in removing the reason it
    will decrement the reference count on the request.

    NOTE: This routine assumes that it is called with the cancel
          spin lock held.

Arguments:

    CurrentOpRequest - Pointer to a pointer to current request for the
                   particular operation.

    IntervalTimer - Pointer to the interval timer for the operation.
                    NOTE: This could be null.

    TotalTimer - Pointer to the total timer for the operation.
                 NOTE: This could be null.

    PDevExt - Pointer to device extension

Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialRundownIrpRefs(
    WDFREQUEST* CurrentOpRequest,
    WDFTIMER IntervalTimer OPTIONAL,
    WDFTIMER TotalTimer OPTIONAL,
    PSERIAL_DEVICE_EXTENSION PDevExt,
    LONG RefType
    )
{
    PREQUEST_CONTEXT reqContext;
    WDFREQUEST request = *CurrentOpRequest;

    reqContext = SerialGetRequestContext(request);

    if(RefType == SERIAL_REF_CANCEL) {

        // Caller is a cancel routine. So just clear the reference.

        SERIAL_CLEAR_REFERENCE( reqContext,  SERIAL_REF_CANCEL );
        reqContext->CancelRoutine = NULL;

    } else {

        // Try to clear the cancelable state.

        SerialClearCancelRoutine(request, TRUE);
    }
    if (IntervalTimer) {

        // Try to cancel the operations interval timer.  If the operation
        // returns true then the timer did have a reference to the
        // request.  Since we've canceled this timer that reference is
        // no longer valid and we can decrement the reference count.
        //
        // If the cancel returns false then this means either of two things:
        //
        // a) The timer has already fired.
        //
        // b) There never was an interval timer.
        //
        // In the case of "b" there is no need to decrement the reference
        // count since the "timer" never had a reference to it.
        //
        // In the case of "a", then the timer itself will be coming
        // along and decrement it's reference.  Note that the caller
        // of this routine might actually be the this timer, so
        // decrement the reference.

        if (SerialCancelTimer(IntervalTimer, PDevExt)) {

            SERIAL_CLEAR_REFERENCE(reqContext,
                                    SERIAL_REF_INT_TIMER);

        } else if(RefType == SERIAL_REF_INT_TIMER) { 

        // caller is the timer

            SERIAL_CLEAR_REFERENCE(reqContext,
                                    SERIAL_REF_INT_TIMER);
        }
    }

    if (TotalTimer) {

        // Try to cancel the operations total timer.  If the operation
        // returns true then the timer did have a reference to the
        // request.  Since we've canceled this timer that reference is
        // no longer valid and we can decrement the reference count.
        //
        // If the cancel returns false then this means either of two things:
        //
        // a) The timer has already fired.
        //
        // b) There never was an total timer.
        //
        // In the case of "b" there is no need to decrement the reference
        // count since the "timer" never had a reference to it.
        //
        // In the case of "a", then the timer itself will be coming
        // along and decrement it's reference.  Note that the caller
        // of this routine might actually be the this timer, so
        // decrement the reference.

        if (SerialCancelTimer(TotalTimer, PDevExt)) {

            SERIAL_CLEAR_REFERENCE(reqContext,
                                    SERIAL_REF_TOTAL_TIMER);

        } else if(RefType == SERIAL_REF_TOTAL_TIMER) {

            SERIAL_CLEAR_REFERENCE(reqContext,
                                    SERIAL_REF_TOTAL_TIMER);
        }
    }
}

/*++

Routine Description:

    This routine is used to either start or queue any requst
    that can be queued in the driver.

Arguments:

    Extension - Points to the serial device extension.

    Request - The request to either queue or start.  In either
          case the request will be marked pending.

    QueueToExamine - The queue the request will be place on if there
                     is already an operation in progress.

    CurrentOpRequest - Pointer to a pointer to the request the is current
                   for the queue.  The pointer pointed to will be
                   set with to Request if what CurrentOpRequest points to
                   is NULL.

    Starter - The routine to call if the queue is empty.

Return Value:


--*/
_Use_decl_annotations_
VOID
SerialStartOrQueue(
    PSERIAL_DEVICE_EXTENSION Extension,
    WDFREQUEST Request,
    WDFQUEUE QueueToExamine,
    WDFREQUEST* CurrentOpRequest,
    PSERIAL_START_ROUTINE Starter
    )
{
    NTSTATUS status;
    PREQUEST_CONTEXT reqContext;
    WDF_REQUEST_PARAMETERS params;

    reqContext = SerialGetRequestContext(Request);

    WDF_REQUEST_PARAMETERS_INIT(&params);

    WdfRequestGetParameters(Request, &params);

    // If this is a write request then take the amount of characters
    // to write and add it to the count of characters to write.

    if (params.Type == WdfRequestTypeWrite) {

        Extension->TotalCharsQueued += reqContext->Length;

    } else if ((params.Type == WdfRequestTypeDeviceControl) &&
               ((params.Parameters.DeviceIoControl.IoControlCode == IOCTL_SERIAL_IMMEDIATE_CHAR) ||
                (params.Parameters.DeviceIoControl.IoControlCode == IOCTL_SERIAL_XOFF_COUNTER))) {

        // We need this in the destroy callback

        reqContext->IoctlCode = params.Parameters.DeviceIoControl.IoControlCode; 

        Extension->TotalCharsQueued++;

    }

    if (IsQueueEmpty(QueueToExamine) &&  !(*CurrentOpRequest)) {

        // There were no current operation.  Mark this one as
        // current and start it up.

        *CurrentOpRequest = Request;

        Starter(Extension);

        return;

    } else {

        // We don't know how long the request will be in the
        // queue.  If it gets cancelled while waiting in the queue, we will
        // be notified by EvtCanceledOnQueue callback so that we can readjust
        // the lenght or free the buffer.
        //
        // We need this in the destroy callback

        reqContext->Extension = Extension; 

        status = WdfRequestForwardToIoQueue(Request,  QueueToExamine);

        if(!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, DBG_READ,
                        "WdfRequestForwardToIoQueue failed%X\r\n",
                        status);

            ASSERTMSG("WdfRequestForwardToIoQueue failed ", FALSE);

            SerialCompleteRequest(Request, status, 0);
        }

        return;
    }
}

/*++

Routine Description:

    Called when the request is cancelled while it's waiting
    on the queue. This callback is used instead of EvtCleanupCallback
    on the request because this one will be called with the
    presentation lock held.


Arguments:

    Queue - Queue in which the request currently waiting
    Request - Request being cancelled


Return Value:

    None.

--*/
_Use_decl_annotations_
VOID
SerialEvtCanceledOnQueue(
    IN WDFQUEUE Queue,
    IN WDFREQUEST Request
    )
{
    PSERIAL_DEVICE_EXTENSION extension = NULL;
    PREQUEST_CONTEXT reqContext;

    UNREFERENCED_PARAMETER(Queue);

    reqContext = SerialGetRequestContext(Request);

    extension = reqContext->Extension;

    // If this is a write request then take the amount of characters
    // to write and subtract it from the count of characters to write.

    if (reqContext->MajorFunction == IRP_MJ_WRITE) {

        extension->TotalCharsQueued -= reqContext->Length;

    } else if (reqContext->MajorFunction == IRP_MJ_DEVICE_CONTROL) {

        // If it's an immediate then we need to decrement the
        // count of chars queued.  If it's a resize then we
        // need to deallocate the pool that we're passing on
        // to the "resizing" routine.

        if (( reqContext->IoctlCode == IOCTL_SERIAL_IMMEDIATE_CHAR) ||
            (reqContext->IoctlCode ==  IOCTL_SERIAL_XOFF_COUNTER)) {

            extension->TotalCharsQueued--;

        } else if (reqContext->IoctlCode ==  IOCTL_SERIAL_SET_QUEUE_SIZE) {

            // We shoved the pointer to the memory into the
            // the type 3 buffer pointer which we KNOW we
            // never use.

            ASSERT(reqContext->Type3InputBuffer);

            ExFreePool(reqContext->Type3InputBuffer);

            reqContext->Type3InputBuffer = NULL;

        }

    }

    SerialCompleteRequest(Request, WdfRequestGetStatus(Request), 0);
}

/*++

Routine Description:

    If the current request is not an IOCTL_SERIAL_GET_COMMSTATUS request and
    there is an error and the application requested abort on errors,
    then cancel the request.

Arguments:

    extension - Pointer to the device context

    Request - Pointer to the WDFREQUEST to test.

Return Value:

    STATUS_SUCCESS or STATUS_CANCELLED.

--*/
_Use_decl_annotations_
NTSTATUS
SerialCompleteIfError(
    PSERIAL_DEVICE_EXTENSION extension,
    WDFREQUEST Request
    )
{
    WDF_REQUEST_PARAMETERS params;
    NTSTATUS status = STATUS_SUCCESS;

    if ((extension->HandFlow.ControlHandShake &
         SERIAL_ERROR_ABORT) && extension->ErrorWord) {

        WDF_REQUEST_PARAMETERS_INIT(&params);

        WdfRequestGetParameters(Request, &params);

        // There is a current error in the driver.  No requests should
        // come through except for the GET_COMMSTATUS.

        if ((params.Type != WdfRequestTypeDeviceControl) ||
                    (params.Parameters.DeviceIoControl.IoControlCode !=  
                    IOCTL_SERIAL_GET_COMMSTATUS)) {
            status = STATUS_CANCELLED;

            SerialCompleteRequest(Request, status, 0);
        }

    }

    return status;
}

/*++

Routine Description:

   This function creates all the timers and DPC objects. All the objects
   are associated with the WDFDEVICE and the callbacks are serialized
   with the device callbacks. Also these objects will be deleted automatically
   when the device is deleted, so there is no need for the driver to explicitly
   delete the objects.

Arguments:

   PDevExt - Pointer to the device extension for the device

Return Value:

    return NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS
SerialCreateTimersAndDpcs(
    IN PSERIAL_DEVICE_EXTENSION pDevExt
    )
{
    WDF_DPC_CONFIG dpcConfig;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES dpcAttributes;
    WDF_OBJECT_ATTRIBUTES timerAttributes;

    // Initialize all the timers used to timeout operations.
    //
    //
    // This timer dpc is fired off if the timer for the total timeout
    // for the read expires.  It will cause the current read to complete.

    WDF_TIMER_CONFIG_INIT(&timerConfig, SerialReadTimeout);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                            &timerAttributes,
                            &pDevExt->ReadRequestTotalTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(ReadRequestTotalTimer) failed. Err=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if the timer for the interval timeout
    // expires.  If no more characters have been read then the
    // dpc routine will cause the read to complete.  However, if
    // more characters have been read then the dpc routine will
    // resubmit the timer.

    WDF_TIMER_CONFIG_INIT(&timerConfig,   SerialIntervalReadTimeout);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                            &timerAttributes,
                            &pDevExt->ReadRequestIntervalTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(ReadRequestIntervalTimer) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if the timer for the total timeout
    // for the write expires.  It will queue a dpc routine that
    // will cause the current write to complete.

    WDF_TIMER_CONFIG_INIT(&timerConfig, SerialWriteTimeout);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                                &timerAttributes,
                                &pDevExt->WriteRequestTotalTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(WriteRequestTotalTimer) failed. Error=%08Xh\n",
                    status);
        return status;
    }

    // This dpc is fired off if the transmit immediate char
    // character times out.  The dpc routine will "grab" the
    // request from the isr and time it out.

    WDF_TIMER_CONFIG_INIT(&timerConfig,   SerialTimeoutImmediate);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                            &timerAttributes,
                            &pDevExt->ImmediateTotalTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(ImmediateTotalTimer) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if the timer used to "timeout" counting
    // the number of characters received after the Xoff ioctl is started
    // expired.

    WDF_TIMER_CONFIG_INIT(&timerConfig, SerialTimeoutXoff);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                            &timerAttributes,
                            &pDevExt->XoffCountTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(XoffCountTimer) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off when a timer expires (after one
    // character time), so that code can be invoked that will
    // check to see if we should lower the RTS line when
    // doing transmit toggling.

    WDF_TIMER_CONFIG_INIT(&timerConfig, SerialInvokePerhapsLowerRTS);

    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfTimerCreate(&timerConfig,
                        &timerAttributes,
                        &pDevExt->LowerRTSTimer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfTimerCreate(LowerRTSTimer) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // Create a DPC to complete read requests.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCompleteWrite);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                        &dpcAttributes,
                        &pDevExt->CompleteWriteDpc);

    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(CompleteWriteDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // Create a DPC to complete read requests.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCompleteRead);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                        &dpcAttributes,
                        &pDevExt->CompleteReadDpc);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(CompleteReadDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if a comm error occurs.  It will
    // cancel all pending reads and writes.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCommError);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                            &dpcAttributes,
                            &pDevExt->CommErrorDpc);

    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(CommErrorDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off when the transmit immediate char
    // character is given to the hardware.  It will simply complete
    // the request.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCompleteImmediate);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                        &dpcAttributes,
                        &pDevExt->CompleteImmediateDpc);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(CompleteImmediateDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if an event occurs and there was
    // a request waiting on that event. A dpc routine will execute
    // that completes the request.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCompleteWait);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                            &dpcAttributes,
                            &pDevExt->CommWaitDpc);
    if (!NT_SUCCESS(status)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(CommWaitDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off if the xoff counter actually runs down
    // to zero.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialCompleteXoff);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                            &dpcAttributes,
                            &pDevExt->XoffCountCompleteDpc);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(XoffCountCompleteDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    // This dpc is fired off only from device level to start off
    // a timer that will queue a dpc to check if the RTS line
    // should be lowered when we are doing transmit toggling.

    WDF_DPC_CONFIG_INIT(&dpcConfig, SerialStartTimerLowerRTS);

    dpcConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&dpcAttributes);
    dpcAttributes.ParentObject = pDevExt->WdfDevice;

    status = WdfDpcCreate(&dpcConfig,
                            &dpcAttributes,
                            &pDevExt->StartTimerLowerRTSDpc);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
                    "WdfDpcCreate(StartTimerLowerRTSDpc) failed. Error=%08Xh\r\n",
                    status);
        return status;
    }

    return status;
}

/*++

Routine Description:

   This function must be called to queue DPC's for the serial driver.

Arguments:

   PDpc - Pointer to the Dpc object

Return Value:

   Kicks up return value from KeInsertQueueDpc()

--*/
_Use_decl_annotations_
BOOLEAN
SerialInsertQueueDpc(WDFDPC PDpc)
{
    // If the specified DPC object is not currently in the queue, WdfDpcEnqueue
    // queues the DPC and returns TRUE.

    return WdfDpcEnqueue(PDpc);
}


/*++

Routine Description:

   This function must be called to set timers for the serial driver.

Arguments:

   Timer - pointer to timer dispatcher object

   DueTime - time at which the timer should expire


Return Value:

   Kicks up return value from KeSetTimerEx()

--*/
_Use_decl_annotations_
BOOLEAN
SerialSetTimer(WDFTIMER Timer, LARGE_INTEGER DueTime)
{
    BOOLEAN result;

    // If the timer object was already in the system timer queue, WdfTimerStart returns TRUE

    result = WdfTimerStart(Timer, DueTime.QuadPart);

    return result;
}

/*++

Routine Description:

   This function cancels all the timers and Dpcs and waits for them
   to run to completion if they are already fired.

Arguments:

   PDevExt - Pointer to the device extension for the device that needs to
             set a timer

Return Value:

--*/
_Use_decl_annotations_
VOID
SerialDrainTimersAndDpcs(
    IN PSERIAL_DEVICE_EXTENSION PDevExt
    )
{
    WdfTimerStop(PDevExt->ReadRequestTotalTimer, TRUE);

    WdfTimerStop(PDevExt->ReadRequestIntervalTimer, TRUE);

    WdfTimerStop(PDevExt->WriteRequestTotalTimer, TRUE);

    WdfTimerStop(PDevExt->ImmediateTotalTimer, TRUE);

    WdfTimerStop(PDevExt->XoffCountTimer, TRUE);

    WdfTimerStop(PDevExt->LowerRTSTimer, TRUE);

    WdfDpcCancel(PDevExt->CompleteWriteDpc, TRUE);

    WdfDpcCancel(PDevExt->CompleteReadDpc, TRUE);

    WdfDpcCancel(PDevExt->CommErrorDpc, TRUE);

    WdfDpcCancel(PDevExt->CompleteImmediateDpc, TRUE);

    WdfDpcCancel(PDevExt->CommWaitDpc, TRUE);

    WdfDpcCancel(PDevExt->XoffCountCompleteDpc, TRUE);

    WdfDpcCancel(PDevExt->StartTimerLowerRTSDpc, TRUE);

    return;
}


/*++

Routine Description:

   This function must be called to cancel timers for the serial driver.

Arguments:

   Timer - pointer to timer dispatcher object

   PDevExt - Pointer to the device extension for the device that needs to
             set a timer

Return Value:

   True if timer was cancelled

--*/
_Use_decl_annotations_
BOOLEAN
SerialCancelTimer(
    IN WDFTIMER Timer,
    IN PSERIAL_DEVICE_EXTENSION PDevExt
    )
{
    UNREFERENCED_PARAMETER(PDevExt);

    return WdfTimerStop(Timer, FALSE);
}

/*++

Routine Description:

   Marks a UART as broken.  This causes the driver stack to stop accepting
   requests and eventually be removed.

Arguments:
   PDevExt - Device extension attached to PDevObj

Return Value:

   None.

--*/
_Use_decl_annotations_
VOID
SerialMarkHardwareBroken(PSERIAL_DEVICE_EXTENSION PDevExt)
{
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "Device is broken. Request a restart...\r\n");

    WdfDeviceSetFailed(PDevExt->WdfDevice, WdfDeviceFailedAttemptRestart);
}

/*++

Routine Description:

    This routine will determine a divisor based on an unvalidated
    baud rate.

Arguments:

    ClockRate - The clock input to the controller.

    DesiredBaud - The baud rate for whose divisor we seek.

    AppropriateDivisor - Given that the DesiredBaud is valid, the
    LONG pointed to by this parameter will be set to the appropriate
    value.  NOTE: The long is undefined if the DesiredBaud is not
    supported.

Return Value:

    This function will return STATUS_SUCCESS if the baud is supported.
    If the value is not supported it will return a status such that
    NT_ERROR(Status) == FALSE.

--*/
_Use_decl_annotations_
NTSTATUS
SerialGetDivisorFromBaud(
                        ULONG ClockRate,
                        LONG DesiredBaud,
                        PSHORT AppropriateDivisor
                        )
{
    NTSTATUS status = STATUS_SUCCESS;
    USHORT calculatedDivisor;
    ULONG denominator;
    ULONG remainder;
    ULONG ulAtualBaudRate=0;
    LONG lBaudRateDevPercBy10=0;

    // Allow up to a 1 percent error in baud rate

   ULONG maxRemain = ClockRate/100;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
                "++SerialGetDivisorFromBaud(clock=%lu, desiredbaudr=%lu)\r\n",
                ClockRate,
                DesiredBaud);

    // Note: the following formula for baud rate divisor on mini Uart on RPi3 
    // is different from 16550 UART forumla.
    // MU_BAUD_REG is 16 bit register which holds clock divisor. 
    // The same divisor value can be written in two parts into normal THR(LS), IER(MS) registers
    //
    // MU_BAUD_REG = (sysclk/(8*baudrate)) - 1

    denominator = DesiredBaud*(ULONG)8;

    // Reject any non-positive bauds.

    if (DesiredBaud <= 0) 
    {
        *AppropriateDivisor = -1;
    } 
    else if ((LONG)denominator < DesiredBaud) 
    {

      //
      // If the desired baud was so huge that it cause the denominator
      // calculation to wrap, don't support it.
      //

      *AppropriateDivisor = -1;

    } else {

      calculatedDivisor = (USHORT)(ClockRate / denominator);
      remainder = ClockRate % denominator;

      TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
                "SerialGetDivisorFromBaud() disv=%d, remndr=%lu\r\n",
                calculatedDivisor,
                remainder);

      // Round up as needed

      if(remainder>maxRemain && remainder<maxRemain*3) 
      {
         calculatedDivisor++;
         denominator=ClockRate/calculatedDivisor;
         remainder = ClockRate % denominator;
      }

      // Only let the remainder calculations effect us if
      // the baud rate is > 9600.

      if (DesiredBaud >= 9600) 
      {
         // If the remainder is less than the maximum remainder (wrt
         // the ClockRate) or the remainder + the maximum remainder is
         // greater than or equal to the ClockRate then assume that the
         // baud is ok.
         //

         if ((remainder >= maxRemain) && ((remainder+maxRemain) < ClockRate)) 
         {
            TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                        "SerialGetDivisorFromBaud(baud=%ld) divisor=%d out of range \r\n",
                        DesiredBaud,
                        calculatedDivisor);

            calculatedDivisor =(USHORT)-1;
            *AppropriateDivisor=-1;
         }
      }

      // Don't support a baud that causes the denominator to
      // be larger than the clock.

      if(denominator > ClockRate) {

         calculatedDivisor = (USHORT)-1;
         *AppropriateDivisor=-1;
         TraceEvents(TRACE_LEVEL_WARNING, DBG_INIT,
                    "SerialGetDivisorFromBaud(baud=%ld) denominator > ClockRate\r\n",
                    DesiredBaud);
      }

      *AppropriateDivisor = calculatedDivisor;
   }

   TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
                "SerialGetDivisorFromBaud(clock=%lu, desiredbaudr=%lu) apprdivisor=%d \r\n",
                ClockRate,
                DesiredBaud,
                *AppropriateDivisor);

   if(*AppropriateDivisor<0) {

      status = STATUS_INVALID_PARAMETER;

   } else {

       ulAtualBaudRate=ClockRate/(8*(*AppropriateDivisor+1));
       lBaudRateDevPercBy10=((DesiredBaud*1000)/ulAtualBaudRate)-1000;

       TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT,
                    "SerialGetDivisorFromBaud() desired baudr=%lu, actual baudr=%lu, deviation=%ld.%u%%\r\n",
                    DesiredBaud,
                    ulAtualBaudRate,
                    lBaudRateDevPercBy10/10,
                    abs(lBaudRateDevPercBy10%10));
   }

   TraceEvents(TRACE_LEVEL_VERBOSE, DBG_INIT,
                "--SerialGetDivisorFromBaud(clock=%lu, desiredbaudr=%lu)=%Xh\r\n",
                ClockRate,
                DesiredBaud,
                status);

   return status;
}

_Use_decl_annotations_
BOOLEAN
IsQueueEmpty(
    IN WDFQUEUE Queue
    )
{
    WDF_IO_QUEUE_STATE queueStatus;

    queueStatus = WdfIoQueueGetState( Queue, NULL, NULL );

    return (WDF_IO_QUEUE_IDLE(queueStatus)) ? TRUE : FALSE;
}

_Use_decl_annotations_
VOID
SerialSetCancelRoutine(
    WDFREQUEST Request,
    PFN_WDF_REQUEST_CANCEL CancelRoutine)
{
    PREQUEST_CONTEXT reqContext = SerialGetRequestContext(Request);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTLS,
                 "++SerialSetCancelRoutine %p\r\n",
                 Request);

    WdfRequestMarkCancelable(Request, CancelRoutine);

    SERIAL_SET_REFERENCE(reqContext, SERIAL_REF_CANCEL);
    
    reqContext->CancelRoutine = CancelRoutine;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTLS, "--SerialSetCancelRoutine\r\n");
    return;
}

_Use_decl_annotations_
NTSTATUS
SerialClearCancelRoutine(
    IN WDFREQUEST Request,
    IN BOOLEAN    ClearReference
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PREQUEST_CONTEXT reqContext = SerialGetRequestContext(Request);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTLS,
                "++SerialClearCancelRoutine %p %x\r\n",
                Request,
                ClearReference);

    if(SERIAL_TEST_REFERENCE(reqContext,  SERIAL_REF_CANCEL))
    {
        status = WdfRequestUnmarkCancelable(Request);
        if (NT_SUCCESS(status)) {

            reqContext->CancelRoutine = NULL;
            if(ClearReference) {

               SERIAL_CLEAR_REFERENCE( reqContext,  SERIAL_REF_CANCEL );

              }
        } else {
             ASSERT(status == STATUS_CANCELLED);
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_IOCTLS,
                "--SerialClearCancelRoutine(%p)=%Xh\r\n",
                Request,
                status);

    return status;
}

_Use_decl_annotations_
VOID
SerialCompleteRequest(
    IN WDFREQUEST Request,
    IN NTSTATUS Status,
    IN ULONG_PTR Info
    )
{
    PREQUEST_CONTEXT reqContext;

    reqContext = SerialGetRequestContext(Request);

    ASSERT(reqContext->RefCount == 0);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_PNP,
                "Complete Request: %p %X 0x%I64x\r\n",
                (Request),
                (Status),
                (Info));

    WdfRequestCompleteWithInformation((Request), (Status), (Info));
}

/*++

Routine Description:

   Query the core clock frequency from RPIQ.

Arguments:

    ClockFrequencyPtr - Pointer to an ULONG representing the clock rate

Return Value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS 
QuerySystemClockFrequency(ULONG* ClockFrequencyPtr)
{
    PAGED_CODE();
    NT_ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    DECLARE_CONST_UNICODE_STRING(rpiqDeviceName, RPIQ_SYMBOLIC_NAME);

    PFILE_OBJECT rpiqFileObject;

    NTSTATUS status = OpenDevice(
        &rpiqDeviceName,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &rpiqFileObject);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "Failed to open handle to RPIQ. (status = %!STATUS!, rpiqDeviceName = %wZ)",
            status,
            &rpiqDeviceName);
        return status;
    }

    // Build input buffer to query clock
    MAILBOX_GET_CLOCK_RATE inputBuffer;
    INIT_MAILBOX_GET_CLOCK_RATE(&inputBuffer, MAILBOX_CLOCK_ID_CORE);

    ULONG_PTR information;
    status = SendIoctlSynchronously(
        rpiqFileObject,
        IOCTL_MAILBOX_PROPERTY,
        &inputBuffer,
        sizeof(inputBuffer),
        &inputBuffer,
        sizeof(inputBuffer),
        FALSE,                  // InternalDeviceIoControl
        &information);
    if (!NT_SUCCESS(status) ||
        (inputBuffer.Header.RequestResponse != RESPONSE_SUCCESS)) {

        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "SendIoctlSynchronously(...IOCTL_MAILBOX_PROPERTY...) failed. (status = %!STATUS!, inputBuffer.Header.RequestResponse = 0x%x)",
            status,
            inputBuffer.Header.RequestResponse);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP,
        "Successfully queried system core clock. (inputBuffer.Rate = %d Hz)",
        inputBuffer.Rate);

    *ClockFrequencyPtr = inputBuffer.Rate;
    return STATUS_SUCCESS;
}

/*++
Routine Description:

    Open a device.

Arguments:

    FileNamePtr    - Pointer to the device filename

    DesiredAccess  - Desired access rights to the device

    ShareAccess    - Type of share access

    FileObjectPPtr - Pointer to the file object, in case of success

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS 
OpenDevice(
    const UNICODE_STRING* FileNamePtr,
    ACCESS_MASK DesiredAccess,
    ULONG ShareAccess,
    FILE_OBJECT** FileObjectPPtr
    )
{
    PAGED_CODE();
    NT_ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(
        &attributes,
        (UNICODE_STRING*)(FileNamePtr),
        OBJ_KERNEL_HANDLE,
        NULL,       // RootDirectory
        NULL);      // SecurityDescriptor

    HANDLE fileHandle;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = ZwCreateFile(
        &fileHandle,
        DesiredAccess,
        &attributes,
        &iosb,
        NULL,                       // AllocationSize
        FILE_ATTRIBUTE_NORMAL,
        ShareAccess,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE,    // CreateOptions
        NULL,                       // EaBuffer
        0);                         // EaLength

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "ZwCreateFile(...) failed. (status = %!STATUS!, FileNamePtr = %wZ, DesiredAccess = 0x%x, ShareAccess = 0x%x)",
            status,
            FileNamePtr,
            DesiredAccess,
            ShareAccess);
        return status;
    }

    status = ObReferenceObjectByHandleWithTag(
        fileHandle,
        DesiredAccess,
        *IoFileObjectType,
        KernelMode,
        POOL_TAG,
        (PVOID*)FileObjectPPtr,
        NULL);   // HandleInformation

    NTSTATUS closeStatus = ZwClose(fileHandle);
    UNREFERENCED_PARAMETER(closeStatus);
    NT_ASSERT(NT_SUCCESS(closeStatus));

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            "ObReferenceObjectByHandleWithTag(...) failed. (status = %!STATUS!)",
            status);
        return status;
    }

    NT_ASSERT(*FileObjectPPtr);
    NT_ASSERT(NT_SUCCESS(status));
    return STATUS_SUCCESS;
}

/*++
Routine Description:

    Synchronously send an IOCTL to a device.

Arguments:

    FileObjectPtr           - File object

    IoControlCode           - IOCTL code

    InputBufferPtr          - Pointer to the input buffer

    InputBufferLength       - Size of the input buffer

    OutputBufferPtr         - Pointer to the output buffer

    OutputBufferLength      - Size of the output buffer

    InternalDeviceIoControl - TRUE  -> sets the IRP's major function code to IRP_MJ_INTERNAL_DEVICE_CONTROL
                              FALSE -> sets the IRP's major function code to IRP_MJ_DEVICE_CONTROL

    InformationPtr          - Request-dependent information

Return value:

    NTSTATUS

--*/
_Use_decl_annotations_
NTSTATUS 
SendIoctlSynchronously(
    FILE_OBJECT* FileObjectPtr,
    ULONG IoControlCode,
    PVOID InputBufferPtr,
    ULONG InputBufferLength,
    PVOID OutputBufferPtr,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    ULONG_PTR* InformationPtr
    )
{
    PAGED_CODE();
    NT_ASSERT(KeGetCurrentIrql() <= PASSIVE_LEVEL);

    KEVENT event;
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    DEVICE_OBJECT* deviceObjectPtr = IoGetRelatedDeviceObject(FileObjectPtr);
    IO_STATUS_BLOCK iosb;
    IRP* irpPtr = IoBuildDeviceIoControlRequest(
        IoControlCode,
        deviceObjectPtr,
        InputBufferPtr,
        InputBufferLength,
        OutputBufferPtr,
        OutputBufferLength,
        InternalDeviceIoControl,
        &event,
        &iosb);
    if (!irpPtr) {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP,
            __FUNCTION__ "IoBuildDeviceIoControlRequest(...) failed. (IoControlCode=0x%x, deviceObjectPtr=%p, FileObjectPtr=%p)",
            IoControlCode,
            deviceObjectPtr,
            FileObjectPtr);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    IO_STACK_LOCATION* irpSp = IoGetNextIrpStackLocation(irpPtr);
    irpSp->FileObject = FileObjectPtr;

    iosb.Status = STATUS_NOT_SUPPORTED;
    NTSTATUS status = IoCallDriver(deviceObjectPtr, irpPtr);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(
            &event,
            Executive,
            KernelMode,
            FALSE,          // Alertable
            NULL);          // Timeout

        status = iosb.Status;
    }

    *InformationPtr = iosb.Information;
    return status;
}
