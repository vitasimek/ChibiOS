/*
    ChibiOS/RT - Copyright (C) 2006-2007 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file LPC214x/serial_lld.c
 * @brief LPC214x low level serial driver code
 * @addtogroup LPC214x_SERIAL
 * @{
 */

#include <ch.h>
#include <serial.h>

#include "board.h"
#include "vic.h"

#if USE_LPC214x_UART0 || defined(__DOXYGEN__)
/** @brief UART0 serial driver identifier.*/
SerialDriver COM1;
#endif

#if USE_LPC214x_UART1 || defined(__DOXYGEN__)
/** @brief UART1 serial driver identifier.*/
SerialDriver COM2;
#endif

/** @brief Driver default configuration.*/
static const SerialDriverConfig default_config = {
  38400,
  LCR_WL8 | LCR_STOP1 | LCR_NOPARITY,
  FCR_TRIGGER0
};

/*===========================================================================*/
/* Low Level Driver local functions.                                         */
/*===========================================================================*/

/**
 * @brief UART initialization.
 * @param[in] u pointer to an UART I/O block
 * @param[in] config the architecture-dependent serial driver configuration
 */
void uart_init(UART *u, const SerialDriverConfig *config) {

  uint32_t div = PCLK / (config->baud_rate << 4);
  u->UART_LCR = config->lcr | LCR_DLAB;
  u->UART_DLL = div;
  u->UART_DLM = div >> 8;
  u->UART_LCR = config->lcr;
  u->UART_FCR = FCR_ENABLE | FCR_RXRESET | FCR_TXRESET | config->fcr;
  u->UART_ACR = 0;
  u->UART_FDR = 0x10;
  u->UART_TER = TER_ENABLE;
  u->UART_IER = IER_RBR | IER_STATUS;
}

/**
 * @brief UART de-initialization.
 * @param[in] u pointer to an UART I/O block
 */
void uart_deinit(UART *u) {

  u->UART_DLL = 1;
  u->UART_DLM = 0;
  u->UART_FDR = 0x10;
  u->UART_IER = 0;
  u->UART_FCR = FCR_RXRESET | FCR_TXRESET;
  u->UART_LCR = 0;
  u->UART_ACR = 0;
  u->UART_TER = TER_ENABLE;
}

/**
 * @brief Error handling routine.
 * @param[in] err UART LSR register value
 * @param[in] sdp communication channel associated to the UART
 */
static void set_error(IOREG32 err, SerialDriver *sdp) {
  sdflags_t sts = 0;

  if (err & LSR_OVERRUN)
    sts |= SD_OVERRUN_ERROR;
  if (err & LSR_PARITY)
    sts |= SD_PARITY_ERROR;
  if (err & LSR_FRAMING)
    sts |= SD_FRAMING_ERROR;
  if (err & LSR_BREAK)
    sts |= SD_BREAK_DETECTED;
  chSysLockFromIsr();
  sdAddFlagsI(sdp, sts);
  chSysUnlockFromIsr();
}

#if defined(__GNU__)
__attribute__((noinline))
#endif
/**
 * @brief Common IRQ handler.
 * @param[in] u pointer to an UART I/O block
 * @param[in] sdp communication channel associated to the UART
 * @note Tries hard to clear all the pending interrupt sources, we dont want to
 *       go through the whole ISR and have another interrupt soon after.
 */
static void serve_interrupt(UART *u, SerialDriver *sdp) {

  while (TRUE) {

    switch (u->UART_IIR & IIR_SRC_MASK) {
    case IIR_SRC_NONE:
      return;
    case IIR_SRC_ERROR:
      set_error(u->UART_LSR, sdp);
      break;
    case IIR_SRC_TIMEOUT:
    case IIR_SRC_RX:
      while (u->UART_LSR & LSR_RBR_FULL) {
        chSysLockFromIsr();
        if (chIQPutI(&sdp->d2.iqueue, u->UART_RBR) < Q_OK)
           sdAddFlagsI(sdp, SD_OVERRUN_ERROR);
        chSysUnlockFromIsr();
      }
      chSysLockFromIsr();
      chEvtBroadcastI(&sdp->d1.ievent);
      chSysUnlockFromIsr();
      break;
    case IIR_SRC_TX:
      {
#if UART_FIFO_PRELOAD > 0
        int i = UART_FIFO_PRELOAD;
        do {
          chSysLockFromIsr();
          msg_t b = chOQGetI(&sdp->d2.oqueue);
          chSysUnlockFromIsr();
          if (b < Q_OK) {
            u->UART_IER &= ~IER_THRE;
            chSysLockFromIsr();
            chEvtBroadcastI(&sdp->d1.oevent);
            chSysUnlockFromIsr();
            break;
          }
          u->UART_THR = b;
        } while (--i);
#else
        chSysLockFromIsr();
        msg_t b = sdRequestDataI(sdp);
        chSysUnlockFromIsr();
        if (b < Q_OK)
          u->UART_IER &= ~IER_THRE;
        else
          u->UART_THR = b;
#endif
      }
    default:
      (void) u->UART_THR;
      (void) u->UART_RBR;
    }
  }
}

#if UART_FIFO_PRELOAD > 0
static void preload(UART *u, SerialDriver *sdp) {

  if (u->UART_LSR & LSR_THRE) {
    int i = UART_FIFO_PRELOAD;
    do {
      chSysLockFromIsr();
      msg_t b = chOQGetI(&sdp->d2.oqueue);
      chSysUnlockFromIsr();
      if (b < Q_OK) {
        chSysLockFromIsr();
        chEvtBroadcastI(&sdp->d1.oevent);
        chSysUnlockFromIsr();
        return;
      }
      u->UART_THR = b;
    } while (--i);
  }
  u->UART_IER |= IER_THRE;
}
#endif

#if USE_LPC214x_UART0 || defined(__DOXYGEN__)
static void notify1(void) {
#if UART_FIFO_PRELOAD > 0

  preload(U0Base, &COM1);
#else
  UART *u = U0Base;

  if (u->UART_LSR & LSR_THRE) {
    chSysLockFromIsr();
    u->UART_THR = chOQGetI(&COM1.sd_oqueue);
    chSysUnlockFromIsr();
  }
  u->UART_IER |= IER_THRE;
#endif
}
#endif

#if USE_LPC214x_UART1 || defined(__DOXYGEN__)
static void notify2(void) {
#if UART_FIFO_PRELOAD > 0

  preload(U1Base, &COM2);
#else
  UART *u = U1Base;

  if (u->UART_LSR & LSR_THRE)
    u->UART_THR = chOQGetI(&COM2.sd_oqueue);
  u->UART_IER |= IER_THRE;
#endif
}
#endif

/*===========================================================================*/
/* Low Level Driver interrupt handlers.                                      */
/*===========================================================================*/

#if USE_LPC214x_UART0 || defined(__DOXYGEN__)
CH_IRQ_HANDLER(UART0IrqHandler) {

  CH_IRQ_PROLOGUE();

  serve_interrupt(U0Base, &COM1);
  VICVectAddr = 0;

  CH_IRQ_EPILOGUE();
}
#endif

#if USE_LPC214x_UART1 || defined(__DOXYGEN__)
CH_IRQ_HANDLER(UART1IrqHandler) {

  CH_IRQ_PROLOGUE();

  serve_interrupt(U1Base, &COM2);
  VICVectAddr = 0;

  CH_IRQ_EPILOGUE();
}
#endif


/*===========================================================================*/
/* Low Level Driver exported functions.                                      */
/*===========================================================================*/

/**
 * Low level serial driver initialization.
 */
void sd_lld_init(void) {

#if USE_LPC214x_UART0
  sdObjectInit(&COM1, NULL, notify1);
  SetVICVector(UART0IrqHandler, LPC214x_UART1_PRIORITY, SOURCE_UART0);
#endif
#if USE_LPC214x_UART1
  sdObjectInit(&COM2, NULL, notify2);
  SetVICVector(UART1IrqHandler, LPC214x_UART2_PRIORITY, SOURCE_UART1);
#endif
}

/**
 * @brief Low level serial driver configuration and (re)start.
 *
 * @param[in] sdp pointer to a @p SerialDriver object
 * @param[in] config the architecture-dependent serial driver configuration.
 *                   If this parameter is set to @p NULL then a default
 *                   configuration is used.
 */
void sd_lld_start(SerialDriver *sdp, const SerialDriverConfig *config) {

  if (config == NULL)
    config = &default_config;

#if USE_LPC214x_UART1
  if (&COM1 == sdp) {
    PCONP = (PCONP & PCALL) | PCUART0;
    uart_init(U0Base, config);
    VICIntEnable = INTMASK(SOURCE_UART0);
    return;
  }
#endif
#if USE_LPC214x_UART2
  if (&COM2 == sdp) {
    PCONP = (PCONP & PCALL) | PCUART1;
    uart_init(U1Base, config);
    VICIntEnable = INTMASK(SOURCE_UART1);
    return;
  }
#endif
}

/**
 * @brief Low level serial driver stop.
 * @details De-initializes the UART, stops the associated clock, resets the
 *          interrupt vector.
 *
 * @param[in] sdp pointer to a @p SerialDriver object
 */
void sd_lld_stop(SerialDriver *sdp) {

#if USE_LPC214x_UART1
  if (&COM1 == sdp) {
    uart_deinit(U0Base);
    PCONP = (PCONP & PCALL) & ~PCUART0;
    VICIntEnClear = INTMASK(SOURCE_UART0);
    return;
  }
#endif
#if USE_LPC214x_UART2
  if (&COM2 == sdp) {
    uart_deinit(U1Base);
    PCONP = (PCONP & PCALL) & ~PCUART1;
    VICIntEnClear = INTMASK(SOURCE_UART1);
    return;
  }
#endif
}

/** @} */
