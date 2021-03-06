/* 
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (c) 2013 China Beijing Armink <armink.ztl@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * File: $Id: mbrtu_m.c,v 1.60 2013/08/17 11:42:56 Armink Add Master Functions $
 */

/* ----------------------- System includes ----------------------------------*/
#include "stdlib.h"
#include "string.h"

/* ----------------------- Platform includes --------------------------------*/
#include "port.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mb_m.h"
#include "mbrtu.h"
#include "mbframe.h"

#include "mbcrc.h"
#include "mbport.h"

#include "modbus.h"

#include "main.h"

#if MB_MASTER_RTU_ENABLED > 0
/* ----------------------- Defines ------------------------------------------*/
#define MB_SER_PDU_SIZE_MIN     4       /*!< Minimum size of a Modbus RTU frame. */
#define MB_SER_PDU_SIZE_MAX     256     /*!< Maximum size of a Modbus RTU frame. */
#define MB_SER_PDU_SIZE_CRC     2       /*!< Size of CRC field in PDU. */
#define MB_SER_PDU_ADDR_OFF     0       /*!< Offset of slave address in Ser-PDU. */
#define MB_SER_PDU_PDU_OFF      1       /*!< Offset of Modbus-PDU in Ser-PDU. */

/* ----------------------- Type definitions ---------------------------------*/
typedef enum
{
    STATE_M_RX_INIT,              /*!< �������� � ������ �������������. */
    STATE_M_RX_IDLE,              /*!< �������� ��������� � ��������� ��������. */
    STATE_M_RX_RCV,               /*!< ����� ������. */
    STATE_M_RX_ERROR,              /*!< �������� �����. */
} eMBMasterRcvState;

typedef enum
{
    STATE_M_TX_IDLE,              /*!< ���������� ��������� � ��������� ��������. */
    STATE_M_TX_XMIT,              /*!< ���������� ��������� � ������ ��������. */
    STATE_M_TX_XFWR,              /*!< ���������� �������� �������� � ������� ������ ������. */
} eMBMasterSndState;

/* ----------------------- Static variables ---------------------------------*/
volatile eMBMasterSndState eSndState;
volatile eMBMasterRcvState eRcvState;

static volatile UCHAR  ucMasterRTUSndBuf[MB_PDU_SIZE_MAX];				// ����� ��� �������� � MODBUS
static volatile UCHAR  ucMasterRTURcvBuf[MB_SER_PDU_SIZE_MAX];			// ����� ��� ����� �� MODBUS
static volatile USHORT usMasterSendPDULength;							// ����� ������ �������� ��� ��������

static volatile UCHAR *pucMasterSndBufferCur;
static volatile USHORT usMasterSndBufferCount;

static volatile USHORT usMasterRcvBufferPos;
static volatile BOOL   xFrameIsBroadcast = FALSE;						// ������� ������������������ ������

static volatile eMBMasterTimerMode eMasterCurTimerMode;

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode
eMBMasterRTUInit(UCHAR ucPort, ULONG ulBaudRate, eMBParity eParity )
{
    eMBErrorCode    eStatus = MB_ENOERR;
    ULONG           usTimerT35_50us;

    ENTER_CRITICAL_SECTION(  );

    // ���� USART ����� 8��� ������ ��� MODBUS
    if( xMBMasterPortSerialInit( ucPort, ulBaudRate, 8, eParity ) != TRUE )
    {
        eStatus = MB_EPORTERR;
    }
    else
    {
    	// ���� �������� ���� 19200 �� ���������� ������������� t3.5 = 1750us
        if( ulBaudRate > 19200 )
        {
            usTimerT35_50us = 35;       /* 1800us. */
        }
        else
        {
            /* The timer reload value for a character is given by:
             *
             * ChTimeValue = Ticks_per_1s / ( Baudrate / 11 )
             *             = 11 * Ticks_per_1s / Baudrate
             *             = 220000 / Baudrate
             * The reload for t3.5 is 1.5 times this value and similary
             * for t3.5.
             */
            usTimerT35_50us = ( 7UL * 220000UL ) / ( 2UL * ulBaudRate );
        }
        // ���� ������� ��� ������� t3.5
        if( xMBMasterPortTimersInit( ( USHORT ) usTimerT35_50us ) != TRUE )
        {
            eStatus = MB_EPORTERR;
        }
    }
    EXIT_CRITICAL_SECTION(  );

    return eStatus;
}

/*************************************************************************
 * eMBMasterRTUStart
 * ���������� �������� � ��������� STATE_M_RX_INIT. ��������� ������ �� t3.5
 * ���� ������ �� �������, �� ��������� �������� � STATE_M_RX_IDLE.
 * ������������� ���� �� ������ ������ ������(t3.5) �� ������ �������� ������.
 *************************************************************************/
void
eMBMasterRTUStart( void )
{
    ENTER_CRITICAL_SECTION(  );

    eRcvState = STATE_M_RX_INIT;
 //   vMBMasterPortSerialEnable( TRUE, FALSE );			// ����� �����
    vMBMasterPortTimersT35Enable(  );					// ����� ������� t3.5 �������

    EXIT_CRITICAL_SECTION(  );
}
/*************************************************************************
 * eMBMasterRTUStop
 * ��������� � ���������� ����� � �������� MODBUS � ���������� �������.
 *************************************************************************/
void
eMBMasterRTUStop( void )
{
    ENTER_CRITICAL_SECTION(  );
    vMBMasterPortSerialEnable( FALSE, FALSE );
    vMBMasterPortTimersDisable(  );
    EXIT_CRITICAL_SECTION(  );
}
/*************************************************************************
 * eMBMasterRTUReceive
 * ������� ����� ������ � �������� �� ����������.
 *************************************************************************/
eMBErrorCode		eMBMasterRTUReceive( UCHAR * pucRcvAddress, UCHAR ** pucFrame, USHORT * pusLength )
{
    eMBErrorCode    eStatus = MB_ERECV;

    ENTER_CRITICAL_SECTION(  );
    assert_param( usMasterRcvBufferPos < MB_SER_PDU_SIZE_MAX );

    // �������� ������ ������ � ��� CRC
    if( ( usMasterRcvBufferPos >= MB_SER_PDU_SIZE_MIN )
        && ( usMBCRC16( ( UCHAR * ) ucMasterRTURcvBuf, usMasterRcvBufferPos ) == 0 ) )
    {
        // Save the address field. All frames are passed to the upper layed and the decision if a frame is used is done there.
        *pucRcvAddress = ucMasterRTURcvBuf[MB_SER_PDU_ADDR_OFF];

        // ����� ����� ������(Modbus-PDU) ��� ���� ������ � CRC.
        *pusLength = ( USHORT )( usMasterRcvBufferPos - MB_SER_PDU_PDU_OFF - MB_SER_PDU_SIZE_CRC );

        // ����� ������ ������(Modbus-PDU)
        *pucFrame = ( UCHAR * ) & ucMasterRTURcvBuf[MB_SER_PDU_PDU_OFF];
    }
    else
    {
        eStatus = MB_EIO;
    }

    //vMBMasterPortTimersDisable( );				// ��� ������ �� �����, ������� ��.
   // xMBMasterPortEventPost(EV_MASTER_FRAME_RECEIVED);

    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}
/*************************************************************************
 * eMBMasterRTUSend
 * �������� ������ � MODBUS
 *************************************************************************/
eMBErrorCode
eMBMasterRTUSend( UCHAR ucSlaveAddress, const UCHAR * pucFrame, USHORT usLength )
{
    eMBErrorCode    eStatus = MB_ESENT;
    USHORT          usCRC16;

    if ( ucSlaveAddress > MB_MASTER_TOTAL_SLAVE_NUM ) return MB_EINVAL;

    ENTER_CRITICAL_SECTION(  );

    // ���� �������� � ������ ��������.
    if( (eRcvState == STATE_M_RX_IDLE || eRcvState == STATE_M_RX_RCV)&& eSndState == STATE_M_TX_IDLE)		//&& eSndState == STATE_M_TX_IDLE ������� �.�. �������� �������� �� �������� ������
    {
        // ������ ����  �� PDU ��� slave address.
        pucMasterSndBufferCur = ( UCHAR * ) pucFrame - 1;
        usMasterSndBufferCount = 1;

        // ��������� � ������ SlaveAddress
        pucMasterSndBufferCur[MB_SER_PDU_ADDR_OFF] = ucSlaveAddress;
        usMasterSndBufferCount += usLength;

        // ������� CRC16
        usCRC16 = usMBCRC16( ( UCHAR * ) pucMasterSndBufferCur, usMasterSndBufferCount );
        ucMasterRTUSndBuf[usMasterSndBufferCount++] = ( UCHAR )( usCRC16 & 0xFF );
        ucMasterRTUSndBuf[usMasterSndBufferCount++] = ( UCHAR )( usCRC16 >> 8 );

        // ����� �������.
        eSndState = STATE_M_TX_XMIT;
        xMBMasterPortSerialPutBUF(( CHAR *)pucMasterSndBufferCur,usMasterSndBufferCount);						// ��������� � ���� ���� ���� �����
        xFrameIsBroadcast = ( ucMasterRTUSndBuf[MB_SER_PDU_ADDR_OFF] == MB_ADDRESS_BROADCAST ) ? TRUE : FALSE;	// ���������� ����������������� �� ?

    }
    else
    {
        eStatus = MB_EIO;					// I/O ������.
        // �������� ��� ��������. ����� ����� ������� ����������� �� ������. ����� ������ STATE_M_RX_RCV
    	USART_TRACE_RED("I/O ������. r:%u t:%u\n",eRcvState,eSndState);

		eRcvState = STATE_M_RX_IDLE;
    }
    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}
/*************************************************************************
 * xMBMasterRTUReceiveFSM
 * ������� ������
 *************************************************************************/
BOOL
xMBMasterRTUReceiveFSM( void )
{
    BOOL            xTaskNeedSwitch = FALSE;
    UCHAR           ucByte;

    // ������ CHAR �� �����.
    //( void )xMBMasterPortSerialGetByte( ( CHAR * ) & ucByte );
    xModbus_Get_SizeAnswer((uint8_t *) &usMasterRcvBufferPos);

    // ���������� � ������� ����� �� ��������
    for (ucByte = 0;ucByte < usMasterRcvBufferPos;ucByte++){
    	xMBMasterPortSerialGetBuf(ucByte, ( CHAR * ) & ucMasterRTURcvBuf[ucByte]);
    }

    switch ( eRcvState )
    {

    case STATE_M_RX_INIT:							// ���� �������� ����� � ��������� ������������� �����, �� ����� ����� ����� ������.
    	// ��������� DMA � ���������� �������� ����� �������� �� ����� ����������.
    	vMBMasterPortTimersT35Enable( );
        break;
    case STATE_M_RX_ERROR:							// � ��������� ������ ������ ���� ���� ���� ����� �� ����������.
    	vMBMasterPortTimersT35Enable( );
        break;

    case STATE_M_RX_IDLE:
    	eSndState = STATE_M_TX_IDLE;
        if(usMasterRcvBufferPos < MB_SER_PDU_SIZE_MAX )	eRcvState = STATE_M_RX_RCV;			// ������� �����.
        else    										eRcvState = STATE_M_RX_ERROR;		// ���� �������� ������ ��� ����. ��������� ������ ������, �� ������ ������.

        xMBMasterPortEventPost(EV_MASTER_FRAME_RECEIVED);			// ��, �������. �� ����� ����� ��������� �������� ������
        break;

    }
    return xTaskNeedSwitch;
}
/*************************************************************************
 * xMBMasterRTUTransmitFSM
 * ���������� ���������� �� ��������� �������� ������
 *************************************************************************/
BOOL
xMBMasterRTUTransmitFSM( void )
{
    BOOL            xNeedPoll = FALSE;

    assert_param( eRcvState == STATE_M_RX_IDLE );

    switch ( eSndState )
    {

    case STATE_M_TX_XMIT:												// ���������� ��������� � ������ ��������.
		eSndState = STATE_M_TX_XFWR;									// ���������� �������� ��������.
		xNeedPoll = TRUE;

 		// ��� ������ ������ �������
		if ( xFrameIsBroadcast == TRUE )
				vMBMasterPortTimersConvertDelayEnable( );				// ������� ��� ������������������ ������.
		else   	vMBMasterPortTimersRespondTimeoutEnable( );				// ������� ��� �� ������������������ ������.

        break;

// ��� ��������� �� ����� ����������. ���������� ��������� ������ � ������ STATE_M_TX_XMIT
    case STATE_M_TX_IDLE:  												// ��������� ��������.
    		// ���� ����� �� ��������� � ����� ����� (��� ����������� RS485)
        break;
    case   STATE_M_TX_XFWR:
        break;
    }

    return xNeedPoll;
}
/*************************************************************************
 * xMBMasterRTUTimerExpired
 * ���������� ��������� �������
 *************************************************************************/
BOOL
xMBMasterRTUTimerExpired(void)
{
	BOOL xNeedPoll = FALSE;

	// �����
	switch (eRcvState)
	{
	case STATE_M_RX_INIT:												// ����� t3.5 �������. ������ �������� ����.
		xNeedPoll = xMBMasterPortEventPost(EV_MASTER_READY);			// Startup ��������. MASTER ����� � ������.
		break;
	case STATE_M_RX_IDLE:												// ����� �� �������� � ������� ���������.
		if (eSndState == STATE_M_TX_XFWR)
			vMBMODBUSPortRxDisable();
		eRcvState = STATE_M_RX_IDLE;
		break;
	case STATE_M_RX_RCV:												// ��� ������� ����� � ������� t3.5 �����. �������� ���������, ��� ��� ������� ����� ����� .
		xNeedPoll = xMBMasterPortEventPost(EV_MASTER_FRAME_RECEIVED);	// ����� ������.
		break;

	case STATE_M_RX_ERROR:												// ������ ��� ��������� ������.
		vMBMasterSetErrorType(EV_ERROR_RECEIVE_DATA);
		xNeedPoll = xMBMasterPortEventPost( EV_MASTER_ERROR_PROCESS );	// ������ ��� ��������� ������.EV_MASTER_ERROR_PROCESS
		break;

	default:															// �������������� ��������� �������.
		assert_param(
				( eRcvState == STATE_M_RX_INIT ) || ( eRcvState == STATE_M_RX_RCV ) ||
				( eRcvState == STATE_M_RX_ERROR ) || ( eRcvState == STATE_M_RX_IDLE ));
		break;
	}
	eRcvState = STATE_M_RX_IDLE;

	// ��������
	switch (eSndState)
	{
	case STATE_M_TX_XFWR:												//  ���������� �������� ��������. ���� ����� �� ������ � ������� ������� �������� �������� �� ������.
		if ( xFrameIsBroadcast == FALSE ) {
			vMBMasterSetErrorType(EV_ERROR_RESPOND_TIMEOUT);
			xNeedPoll = xMBMasterPortEventPost(EV_MASTER_ERROR_PROCESS);
		}
		break;
	default:															// �������������� ��������� �������.
		assert_param(
				( eSndState == STATE_M_TX_XFWR ) || ( eSndState == STATE_M_TX_IDLE ));
		break;
	}
	eSndState = STATE_M_TX_IDLE;


	vMBMasterPortTimersDisable( );										// ������������� ������.
	/* If timer mode is convert delay, the master event then turns EV_MASTER_EXECUTE status. */
	if (eMasterCurTimerMode == MB_TMODE_CONVERT_DELAY) {
		xNeedPoll = xMBMasterPortEventPost( EV_MASTER_EXECUTE );
	}

	return xNeedPoll;
}

/* Get Modbus Master send RTU's buffer address pointer.*/
void vMBMasterGetRTUSndBuf( UCHAR ** pucFrame )
{
	*pucFrame = ( UCHAR * ) ucMasterRTUSndBuf;
}

/* Get Modbus Master send PDU's buffer address pointer.*/
void vMBMasterGetPDUSndBuf( UCHAR ** pucFrame )
{
	*pucFrame = ( UCHAR * ) &ucMasterRTUSndBuf[MB_SER_PDU_PDU_OFF];
}

/* Set Modbus Master send PDU's buffer length.*/
void vMBMasterSetPDUSndLength( USHORT SendPDULength )
{
	usMasterSendPDULength = SendPDULength;
}

/* Get Modbus Master send PDU's buffer length.*/
USHORT usMBMasterGetPDUSndLength( void )
{
	return usMasterSendPDULength;
}

/* Set Modbus Master current timer mode.*/
void vMBMasterSetCurTimerMode( eMBMasterTimerMode eMBTimerMode )
{
	eMasterCurTimerMode = eMBTimerMode;
}

/* The master request is broadcast? */
BOOL xMBMasterRequestIsBroadcast( void ){
	return xFrameIsBroadcast;
}
#endif

