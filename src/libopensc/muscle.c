/*
 * muscle.c: Support for MuscleCard Applet from musclecard.com 
 *
 * Copyright (C) 2006, Identity Alliance, Thomas Harning <support@identityalliance.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include "muscle.h"
#include "internal.h"

#include <string.h>

#define MSC_MAX_WRITE_UNIT 255
#define MSC_MAX_READ_UNIT 246

#define MSC_MAX_CRYPTINIT_DATA (255 - 5)
#define MSC_MAX_CRYPTPROCESS_DATA (255 - 3)

#define MSC_RSA_PUBLIC		0x01
#define MSC_RSA_PRIVATE 	0x02
#define MSC_RSA_PRIVATE_CRT	0x03
#define MSC_DSA_PUBLIC		0x04
#define MSC_DSA_PRIVATE 	0x05

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#if defined(_WIN32) || defined(_WIN64)
static unsigned long bswap_32(unsigned long x)
{
    unsigned long res = x % 256;
    x /= 256;
    res = 256 * res + x % 256;
    x /= 256;
    res = 256 * res + x % 256;
    x /= 256;
    return 256 * res + x % 256;
}
static unsigned short bswap_16(unsigned short x)
{
    return 256 * (x % 256) + (x / 256);
}
#define BIG_ENDIAN 1
#else
#include <endian.h>
#include <byteswap.h>
#endif

int msc_list_objects(sc_card_t* card, u8 next, mscfs_file_t* file) {
	sc_apdu_t apdu;
	u8 fileData[14];
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2, 0x58, next, 0x00);
	apdu.cla = 0xB0;
	apdu.le = 14;
	apdu.resplen = 14;
	apdu.resp = fileData;
	r = sc_transmit_apdu(card, &apdu);
	if (r)
		return r;
	
	if(apdu.sw1 == 0x9C && apdu.sw2 == 0x12) {
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r)
		return r;
	if(apdu.resplen == 0) /* No more left */
		return 0;
	if (apdu.resplen != 14) {
		sc_error(card->ctx, "expected 14 bytes, got %d.\n", apdu.resplen);
		return SC_ERROR_UNKNOWN_DATA_RECEIVED;
	}
	memcpy(file->objectId, fileData, 4);
	file->size = *(int*)(fileData + 4);
	file->read = *(short*)(fileData + 8);
	file->write = *(short*)(fileData + 10);
	file->delete = *(short*)(fileData + 12);
	
	if(BIG_ENDIAN) {
		file->size = bswap_32(file->size);
		file->read = bswap_16(file->read);
		file->write = bswap_16(file->write);
		file->delete = bswap_16(file->delete);
	}
	return 1;
}

int msc_partial_read_object(sc_card_t *card, unsigned int le_objectId, int offset, u8 *data, size_t dataLength)
{
	u8 buffer[9];
	sc_apdu_t apdu;
	int r;
	unsigned int le_offset = offset;
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x56, 0x00, 0x00);
	
	if(BIG_ENDIAN)
		le_offset = bswap_32(le_offset);
	if (card->ctx->debug >= 2)
		sc_debug(card->ctx, "READ: Offset: %x\tLength: %i\n", le_offset, dataLength);
	memcpy(buffer, &le_objectId, 4);
	memcpy(buffer + 4, &le_offset, 4);
	buffer[8] = (u8)dataLength;
	apdu.data = buffer;
	apdu.datalen = 9;
	apdu.lc = 9;
	apdu.le = dataLength;
	apdu.resplen = dataLength;
	apdu.resp = data; 
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		return dataLength;
	if(apdu.sw1 == 0x9C) {
		if(apdu.sw2 == 0x07) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_FILE_NOT_FOUND);
		} else if(apdu.sw2 == 0x06) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_ALLOWED);
		} else if(apdu.sw2 == 0x0F) {
			/* GUESSED */
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		}
	}
	if (card->ctx->debug >= 2) {
		sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
		     apdu.sw1, apdu.sw2);
	}
	return dataLength;
	
}

int msc_read_object(sc_card_t *card, unsigned int objectId, int offset, u8 *data, size_t dataLength)
{
	int r;
	size_t i;
	for(i = 0; i < dataLength; i += MSC_MAX_WRITE_UNIT) {
		r = msc_partial_read_object(card, objectId, offset + i, data + i, MIN(dataLength - i, MSC_MAX_WRITE_UNIT));
		SC_TEST_RET(card->ctx, r, "Error in partial object read");
	}
	return dataLength;
}

int msc_zero_object(sc_card_t *card, unsigned int objectId, size_t dataLength)
{
	u8 zeroBuffer[MSC_MAX_WRITE_UNIT];
	size_t i;
	memset(zeroBuffer, 0, MSC_MAX_WRITE_UNIT);
	for(i = 0; i < dataLength; i += MSC_MAX_WRITE_UNIT) {
		int r = msc_partial_update_object(card, objectId, i, zeroBuffer, MIN(dataLength - i, MSC_MAX_WRITE_UNIT));
		SC_TEST_RET(card->ctx, r, "Error in zeroing file update");
	}
	return 0;
}

int msc_create_object(sc_card_t *card, unsigned int objectId, size_t objectSize, unsigned short read, unsigned short write, unsigned short deletion)
{
	u8 buffer[14];
	sc_apdu_t apdu;
	unsigned short readAcl = read, writeAcl = write, deleteAcl = deletion;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x5A, 0x00, 0x00);
	apdu.lc = 14;
	apdu.data = buffer,
	apdu.datalen = 14;
	
	if(BIG_ENDIAN) {
		objectSize = bswap_32(objectSize);
		readAcl = bswap_16(readAcl);
		writeAcl = bswap_16(writeAcl);
		deleteAcl = bswap_16(deleteAcl);
	}
	memcpy(buffer, &objectId, 4);
	memcpy(buffer + 4, &objectSize, 4);
	memcpy(buffer + 8, &readAcl, 2);
	memcpy(buffer + 10, &writeAcl, 2);
	memcpy(buffer + 12, &deleteAcl, 2);
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		return BIG_ENDIAN ? bswap_32(objectSize) : objectSize;
	if(apdu.sw1 == 0x9C) {
		if(apdu.sw2 == 0x01) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_MEMORY_FAILURE);
		} else if(apdu.sw2 == 0x08) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_FILE_ALREADY_EXISTS);
		} else if(apdu.sw2 == 0x06) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_ALLOWED);
		}
	}
	if (card->ctx->debug >= 2) {
		sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
		     apdu.sw1, apdu.sw2);
	}
	msc_zero_object(card, objectId, BIG_ENDIAN ? bswap_32(objectSize) : objectSize);
	return BIG_ENDIAN ? bswap_32(objectSize) : objectSize;
}

/* Update up to 246 bytes */
int msc_partial_update_object(sc_card_t *card, unsigned int le_objectId, int offset, const u8 *data, size_t dataLength)
{
	u8 buffer[256];
	sc_apdu_t apdu;
	unsigned int le_offset;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x54, 0x00, 0x00);
	apdu.lc = dataLength + 9;
	le_offset = offset;
	if(BIG_ENDIAN)
		le_offset = bswap_32(le_offset);
	if (card->ctx->debug >= 2)
		sc_debug(card->ctx, "WRITE: Offset: %x\tLength: %i\n", le_offset, dataLength);
	memcpy(buffer, &le_objectId, 4);
	memcpy(buffer + 4, &le_offset, 4);
	buffer[8] = (u8)dataLength;
	memcpy(buffer + 9, data, dataLength);
	apdu.data = buffer;
	apdu.datalen = apdu.lc;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		return dataLength;
	if(apdu.sw1 == 0x9C) {
		if(apdu.sw2 == 0x07) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_FILE_NOT_FOUND);
		} else if(apdu.sw2 == 0x06) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_ALLOWED);
		} else if(apdu.sw2 == 0x0F) {
			/* GUESSED */
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS);
		}
	}
	if (card->ctx->debug >= 2) {
		sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
		     apdu.sw1, apdu.sw2);
	}
	return dataLength;
}

int msc_update_object(sc_card_t *card, unsigned int objectId, int offset, const u8 *data, size_t dataLength)
{
	int r;
	size_t i;
	for(i = 0; i < dataLength; i += MSC_MAX_READ_UNIT) {
		r = msc_partial_update_object(card, objectId, offset + i, data + i, MIN(dataLength - i, MSC_MAX_READ_UNIT));
		SC_TEST_RET(card->ctx, r, "Error in partial object update");
	}
	return dataLength;
}

int msc_delete_object(sc_card_t *card, unsigned int objectId, int zero)
{
	sc_apdu_t apdu;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x52, 0x00, zero ? 0x01 : 0x00);
	apdu.lc = 4;
	apdu.data = (u8*)&objectId;
	apdu.datalen = 4;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		return 0;
	if(apdu.sw1 == 0x9C) {
		if(apdu.sw2 == 0x07) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_FILE_NOT_FOUND);
		} else if(apdu.sw2 == 0x06) {
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_NOT_ALLOWED);
		}
	}
	if (card->ctx->debug >= 2) {
		sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
		     apdu.sw1, apdu.sw2);
	}
	return 0;
}

int msc_select_applet(sc_card_t *card, u8 *appletId, size_t appletIdLength)
{
	sc_apdu_t apdu;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xA4, 4, 0);
	apdu.lc = appletIdLength;
	apdu.data = appletId;
	apdu.datalen = appletIdLength;
	apdu.resplen = 0;
	apdu.le = 0;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
		return 1;
	
	SC_FUNC_RETURN(card->ctx, 2,  SC_ERROR_CARD_CMD_FAILED);
}

int msc_verify_pin(sc_card_t *card, int pinNumber, const u8 *pinValue, int pinLength, int *tries)
{
	sc_apdu_t apdu;
	int r;

	msc_verify_pin_apdu(card, &apdu, pinNumber, pinValue, pinLength);
	if(tries)
		*tries = -1;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		return 0;
	} else if(apdu.sw1 == 0x63) { /* Invalid auth */
		if(tries)
			*tries = apdu.sw2 & 0x0F;
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x9C && apdu.sw2 == 0x02) {
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x69 && apdu.sw2 == 0x83) {
		SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_AUTH_METHOD_BLOCKED);
	}
	
	SC_FUNC_RETURN(card->ctx, 2,  SC_ERROR_PIN_CODE_INCORRECT);
}

/* USE ISO_VERIFY due to tries return */
void msc_verify_pin_apdu(sc_card_t *card, sc_apdu_t *apdu, int pinNumber, const u8 *pinValue, int pinLength)
{
	/* FORCE PIN TO END AFTER LAST NULL */
	for(; pinLength > 0; pinLength--) {
		if(pinValue[pinLength - 1]) break;
	}
	sc_format_apdu(card, apdu, SC_APDU_CASE_3_SHORT, 0x42, pinNumber, 0);
	apdu->lc = pinLength;
	apdu->data = pinValue;
	apdu->datalen = pinLength;
}

int msc_unblock_pin(sc_card_t *card, int pinNumber, const u8 *pukValue, int pukLength, int *tries)
{
	sc_apdu_t apdu;
	int r;

	msc_unblock_pin_apdu(card, &apdu, pinNumber, pukValue, pukLength);
	if(tries)
		*tries = -1;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		return 0;
	} else if(apdu.sw1 == 0x63) { /* Invalid auth */
		if(tries)
			*tries = apdu.sw2 & 0x0F;
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x9C && apdu.sw2 == 0x02) {
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x69 && apdu.sw2 == 0x83) {
		SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_AUTH_METHOD_BLOCKED);
	}
	
	SC_FUNC_RETURN(card->ctx, 2,  SC_ERROR_PIN_CODE_INCORRECT);
}

void msc_unblock_pin_apdu(sc_card_t *card, sc_apdu_t *apdu, int pinNumber, const u8 *pukValue, int pukLength)
{
	sc_format_apdu(card, apdu, SC_APDU_CASE_3_SHORT, 0x46, pinNumber, 0);
	apdu->lc = pukLength;
	apdu->data = pukValue;
	apdu->datalen = pukLength;
}

int msc_change_pin(sc_card_t *card, int pinNumber, const u8 *pinValue, int pinLength, const u8 *newPin, int newPinLength, int *tries)
{
	sc_apdu_t apdu;
	int r;

	msc_change_pin_apdu(card, &apdu, pinNumber, pinValue, pinLength, newPin, newPinLength);
	if(tries)
		*tries = -1;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		return 0;
	} else if(apdu.sw1 == 0x63) { /* Invalid auth */
		if(tries)
			*tries = apdu.sw2 & 0x0F;
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x9C && apdu.sw2 == 0x02) {
		SC_FUNC_RETURN(card->ctx, 0,  SC_ERROR_PIN_CODE_INCORRECT);
	} else if(apdu.sw1 == 0x69 && apdu.sw2 == 0x83) {
		SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_AUTH_METHOD_BLOCKED);
	}
	
	SC_FUNC_RETURN(card->ctx, 2,  SC_ERROR_PIN_CODE_INCORRECT);
}

/* USE ISO_VERIFY due to tries return */
void msc_change_pin_apdu(sc_card_t *card, sc_apdu_t *apdu, int pinNumber, const u8 *pinValue, int pinLength, const u8 *newPin, int newPinLength)
{
	u8 pinData[512]; /* Absolute max size 255 * 2 + 2 */
	u8 *ptr = pinData;

	sc_format_apdu(card, apdu, SC_APDU_CASE_3_SHORT, 0x44, pinNumber, 0);
	*ptr = pinLength;
	ptr++;
	memcpy(ptr, pinValue, pinLength);
	ptr += pinLength;
	*ptr = newPinLength;
	ptr++;
	memcpy(ptr, newPin, newPinLength);
	ptr += newPinLength;
	apdu->lc = pinLength + newPinLength + 2;
	apdu->datalen = apdu->lc;
	apdu->data = pinData;
}

int msc_get_challenge(sc_card_t *card, short dataLength, short seedLength, u8 *seedData, u8* outputData)
{
	sc_apdu_t apdu;
	int r, location, cse, len;
	short dataLength_le = dataLength;
	short seedLength_le = seedLength;
	u8 *buffer, *ptr;
	
	location = (dataLength < 255) ? 1 : 2; /* 1 == APDU, 2 == (seed in 0xFFFFFFFE, out in 0xFFFFFFFF) */
	cse = (location == 1) ? SC_APDU_CASE_4_SHORT : SC_APDU_CASE_3_SHORT;
	len = seedLength + 4;

	assert(seedLength < 251);
	assert(dataLength < 255); /* Output buffer doesn't seem to operate as desired.... nobody can read/delete */
	if(BIG_ENDIAN) {
		dataLength_le = bswap_16(dataLength_le);
		seedLength_le = bswap_16(seedLength_le);
	}
	
	buffer = malloc(len);
	if(!buffer) SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
	ptr = buffer;
	memcpy(ptr, &dataLength_le, 2);
	ptr+=2;
	memcpy(ptr, &seedLength_le, 2);
	ptr+=2;
	if(seedLength > 0) {
		memcpy(ptr, seedData, seedLength);
	}
	sc_format_apdu(card, &apdu, cse, 0x72, 0x00, location);
	apdu.data = buffer;
	apdu.datalen = len;
	apdu.lc = len;
	
	if(location == 1) {
		u8* outputBuffer = malloc(dataLength + 2);
		if(outputBuffer == NULL) SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
		apdu.le = dataLength + 2;
		apdu.resp = outputBuffer;
		apdu.resplen = dataLength + 2;
	}
	r = sc_transmit_apdu(card, &apdu);
	if(location == 1) {
		memcpy(outputData, apdu.resp + 2, dataLength);
		free(apdu.resp);
	}
	free(buffer);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(location == 1) {
		if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
			return dataLength;
		} else {
			r = sc_check_sw(card, apdu.sw1, apdu.sw2);
			if (r) {
				if (card->ctx->debug >= 2) {
					sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
					     apdu.sw1, apdu.sw2);
				}
				SC_FUNC_RETURN(card->ctx, 0, r);
			}
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
		}
	} else {
		if(apdu.sw1 != 0x90 || apdu.sw2 != 0x00) {
			r = sc_check_sw(card, apdu.sw1, apdu.sw2);
			if (r) {
				if (card->ctx->debug >= 2) {
					sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
					     apdu.sw1, apdu.sw2);
				}
				SC_FUNC_RETURN(card->ctx, 0, r);
			}
			SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
		}
		r = msc_read_object(card, 0xFFFFFFFFul, 2, outputData, dataLength);
		if(r < 0)
			SC_FUNC_RETURN(card->ctx, 0, r);
		r = msc_delete_object(card, 0xFFFFFFFFul,0);
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
}

int msc_generate_keypair(sc_card_t *card, int privateKey, int publicKey, int algorithm, int keySize, int options)
{
	sc_apdu_t apdu;
	u8 buffer[256];
	u8 *ptr = buffer;
	int r;
	unsigned short prRead = 0xFFFF, prWrite = 0x0002, prCompute = 0x0002,
		puRead = 0x0000, puWrite = 0x0002, puCompute = 0x0000;

	assert(privateKey <= 0x0F && publicKey <= 0x0F);
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x30, privateKey, publicKey);
	if(BIG_ENDIAN) {
		keySize = bswap_16(keySize);
		prRead = bswap_16(prRead);
		prWrite = bswap_16(prWrite);
		prCompute = bswap_16(prCompute);
		puRead = bswap_16(puRead);
		puWrite = bswap_16(puWrite);
		puCompute = bswap_16(puCompute);
	}
	*ptr = algorithm; ptr++;
	
	memcpy(ptr, &keySize, 2);
	ptr+=2;
	
	memcpy(ptr, &prRead, 2);
	ptr+=2;
	memcpy(ptr, &prWrite, 2);
	ptr+=2;
	memcpy(ptr, &prCompute, 2);
	ptr+=2;
	
	memcpy(ptr, &puRead, 2);
	ptr+=2;
	memcpy(ptr, &puWrite, 2);
	ptr+=2;
	memcpy(ptr, &puCompute, 2);
	ptr+=2;
	
	*ptr = 0; /* options; -- no options for now, they need extra data */
	
	apdu.data = buffer;
	apdu.datalen = 16;
	apdu.lc = 16;
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}

int msc_extract_key(sc_card_t *card, 
			int keyLocation)
{
	sc_apdu_t apdu;
	u8 encoding = 0;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x34, keyLocation, 0x00);
	apdu.data = &encoding;
	apdu.datalen = 1;
	apdu.lc = 1;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}

int msc_extract_rsa_public_key(sc_card_t *card, 
			int keyLocation,
			int* modLength, 
			u8** modulus,
			int* expLength,
			u8** exponent)
{
	int r;
	const int buffer_size = 1024;
	u8 buffer[1024]; /* Should be plenty... */
	int fileLocation = 1;

	assert(modLength && expLength && modulus && exponent);
	r = msc_extract_key(card, keyLocation);
	if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
	
	/* Read keyType, keySize, and what should be the modulus size */	
	r = msc_read_object(card, 0xFFFFFFFFul, fileLocation, buffer, 5);
	fileLocation += 5;
	if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
	
	if(buffer[0] != MSC_RSA_PUBLIC) SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_UNKNOWN_DATA_RECEIVED);
	*modLength = (buffer[3] << 8) | buffer[4];
	/* Read the modulus and the exponent length */
	assert(*modLength + 2 < buffer_size);
	
	r = msc_read_object(card, 0xFFFFFFFFul, fileLocation, buffer, *modLength + 2);
	fileLocation += *modLength + 2;
	if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
	
	*modulus = malloc(*modLength);
	if(!*modulus) SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
	memcpy(*modulus, buffer, *modLength);
	*expLength = (buffer[*modLength] << 8) | buffer[*modLength + 1];
	assert(*expLength < buffer_size);
	r = msc_read_object(card, 0xFFFFFFFFul, fileLocation, buffer, *expLength);
	if(r < 0) {
		free(*modulus); *modulus = NULL;
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	*exponent = malloc(*expLength);
	if(!*exponent) {
		free(*modulus);
		SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
	}
	memcpy(*exponent, buffer, *expLength);
	return 0;
}



/* For the moment, only support streaming data to the card 
	in blocks, not through file IO */
int msc_compute_crypt_init(sc_card_t *card, 
			int keyLocation,
			int cipherMode,
			int cipherDirection,
			const u8* initData,
			u8* outputData,
			size_t dataLength,
			size_t* outputDataLength)
{
	sc_apdu_t apdu;
	u8 buffer[255];
	u8 *ptr;
	int r;

	u8 outputBuffer[255];
	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x36, keyLocation, 0x01); /* Init */
	apdu.data = buffer;
	apdu.datalen = dataLength + 5;
	apdu.lc = dataLength + 5;
	
	memset(outputBuffer, 0, sizeof(outputBuffer));
	apdu.resp = outputBuffer;
	apdu.resplen = dataLength + 2;
	apdu.le = dataLength + 2;
	ptr = buffer;
	*ptr = cipherMode; ptr++;
	*ptr = cipherDirection; ptr++;
	*ptr = 0x01; ptr++; /* DATA LOCATION: APDU */
	*ptr = (dataLength >> 8) & 0xFF; ptr++;
	*ptr = dataLength & 0xFF; ptr++;
	memcpy(ptr, initData, dataLength);
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		short receivedData = outputBuffer[0] << 8 | outputBuffer[1];
		 *outputDataLength = receivedData;
		*outputDataLength = 0;
		assert(receivedData <= 255);
		memcpy(outputData, outputBuffer + 2, receivedData);
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "init: got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}

int msc_compute_crypt_process(
			sc_card_t *card, 
			int keyLocation,
			const u8* inputData,
			u8* outputData,
			size_t dataLength,
			size_t* outputDataLength)
{
	sc_apdu_t apdu;
	u8 buffer[255];
	u8 outputBuffer[255];
	u8 *ptr;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x36, keyLocation, 0x02); /* Process */
	
	apdu.data = buffer;
	apdu.datalen = dataLength + 3;
	apdu.lc = dataLength + 3;
/* Specs say crypt returns data all the time??? But... its not implemented that way */
	
	memset(outputBuffer, 0, sizeof(outputBuffer));
	apdu.resp = outputBuffer;
	apdu.resplen = 255;
	apdu.le = dataLength;
	ptr = buffer;
	*ptr = 0x01; ptr++; /* DATA LOCATION: APDU */
	*ptr = (dataLength >> 8) & 0xFF; ptr++;
	*ptr = dataLength & 0xFF; ptr++;
	memcpy(ptr, inputData, dataLength);
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		short receivedData = outputBuffer[0] << 8 | outputBuffer[1];
		 *outputDataLength = receivedData;
		*outputDataLength = 0;
		assert(receivedData <= 255);
		memcpy(outputData, outputBuffer + 2, receivedData);
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "process: got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}

int msc_compute_crypt_final(
			sc_card_t *card, 
			int keyLocation,
			const u8* inputData,
			u8* outputData,
			size_t dataLength,
			size_t* outputDataLength)
{
	sc_apdu_t apdu;
	u8 buffer[255];
	u8 outputBuffer[255];
	u8 *ptr;
	int r;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x36, keyLocation, 0x03); /* Final */
	
	apdu.data = buffer;
	apdu.datalen = dataLength + 3;
	apdu.lc = dataLength + 3;
	
	memset(outputBuffer, 0, sizeof(outputBuffer));
	apdu.resp = outputBuffer;
	apdu.resplen = 255;
	apdu.le = 255;
	ptr = buffer;
	*ptr = 0x01; ptr++; /* DATA LOCATION: APDU */
	*ptr = (dataLength >> 8) & 0xFF; ptr++;
	*ptr = dataLength & 0xFF; ptr++;
	memcpy(ptr, inputData, dataLength);
	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		short receivedData = outputBuffer[0] << 8 | outputBuffer[1];
		*outputDataLength = receivedData;
		assert(receivedData <= 255);
		memcpy(outputData, outputBuffer + 2, receivedData);
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "final: got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}

int msc_compute_crypt(sc_card_t *card, 
			int keyLocation,
			int cipherMode,
			int cipherDirection,
			const u8* data,
			u8* outputData,
			size_t dataLength,
			size_t outputDataLength)
{
	int left = dataLength;
	const u8* inPtr = data;
	u8* outPtr = outputData;
	int toSend;
	int r;

	size_t received = 0;
	assert(outputDataLength >= dataLength);
	
	/* Don't send data during init... apparently current version does not support it */
	toSend = 0;
	r = msc_compute_crypt_init(card, 
		keyLocation, 
		cipherMode, 
		cipherDirection, 
		inPtr, 
		outPtr, 
		toSend,
		&received);
	if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
	left -= toSend;
	inPtr += toSend;
	outPtr += received;
	while(left > MSC_MAX_CRYPTPROCESS_DATA) {
		toSend = MIN(left, MSC_MAX_CRYPTINIT_DATA);
		r = msc_compute_crypt_process(card,
			keyLocation,
			inPtr,
			outPtr,
			toSend,
			&received);
		if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
		left -= toSend;
		inPtr += toSend;
		outPtr += received;
	}
	toSend = MIN(left, MSC_MAX_CRYPTINIT_DATA);
	r = msc_compute_crypt_final(card,
		keyLocation,
		inPtr,
		outPtr,
		toSend,
		&received);
	if(r < 0) SC_FUNC_RETURN(card->ctx, 0, r);
	left -= toSend;
	inPtr += toSend;
	outPtr += received;
	return outPtr - outputData; /* Amt received */
}

/* USED IN KEY ITEM WRITING */
#define CPYVAL(valName) \
	length = BIG_ENDIAN ? bswap_16(data->valName ## Length) : data->valName ## Length; \
	memcpy(p, &length, 2); p+= 2; \
	memcpy(p, data->valName ## Value, data->valName ## Length); p+= data->valName ## Length

int msc_import_key(sc_card_t *card,
	int keyLocation,
	sc_cardctl_muscle_key_info_t *data)
{
	unsigned short read = 0xFFFF,
		write = 0x0002,
		use = 0x0002,
		keySize = data->keySize;
	int bufferSize = 0;
	u8 *buffer, *p;
	unsigned int objectId;
	u8 apduBuffer[6];
	sc_apdu_t apdu;
	int r;

	assert(data->keyType == 0x02 || data->keyType == 0x03);
	if(data->keyType == 0x02) {
		if( (data->pLength == 0 || !data->pValue)
		|| (data->modLength == 0 || !data->modValue))
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS); 
	} else if(data->keyType == 0x03) {
		if( (data->pLength == 0 || !data->pValue)
		|| (data->qLength == 0 || !data->qValue)
		|| (data->pqLength == 0 || !data->pqValue)
		|| (data->dp1Length == 0 || !data->dp1Value)
		|| (data->dq1Length == 0 || !data->dq1Value))
			SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS); 
	} else {
		SC_FUNC_RETURN(card->ctx, 2, SC_ERROR_INVALID_ARGUMENTS)
	}
	
	if(BIG_ENDIAN) {
		read = bswap_16(read);
		write = bswap_16(write);
		use = bswap_16(use);
		keySize = bswap_16(keySize);
	}
	
	if(data->keyType == 0x02) {
		bufferSize = 4 + 4 + data->pLength + data->modLength;
	} else if(data->keyType == 0x03) {
		bufferSize = 4 + 10
			+ data->pLength + data->qLength + data->pqLength
			+ data->dp1Length + data->dq1Length;
	}
	buffer = malloc(bufferSize);
	if(!buffer) SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_OUT_OF_MEMORY);
	p = buffer;
	*p = 0x00; p++; /* Encoding plain */
	*p = data->keyType; p++; /* RSA_PRIVATE */
	memcpy(p, &keySize, 2); p+=2; /* key size */
	
	if(data->keyType == 0x02) {
		unsigned int length;
		CPYVAL(mod);
		CPYVAL(p);
	} else if(data->keyType == 0x03) {
		unsigned int length;
		CPYVAL(p);
		CPYVAL(q);
		CPYVAL(pq);
		CPYVAL(dp1);
		CPYVAL(dq1);
	}
	objectId = 0xFFFFFFFEul;
	if(BIG_ENDIAN) {
		objectId = bswap_32(objectId);
	}
	r = msc_create_object(card, objectId, bufferSize, 0x02, 0x02, 0x02);
	if(r < 0) { 
		if(r == SC_ERROR_FILE_ALREADY_EXISTS) {
			r = msc_delete_object(card, objectId, 0);
			if(r < 0) {
				free(buffer);
				SC_FUNC_RETURN(card->ctx, 2, r);
			}
			r = msc_create_object(card, objectId, bufferSize, 0x02, 0x02, 0x02);
			if(r < 0) {
				free(buffer);
				SC_FUNC_RETURN(card->ctx, 2, r);
			}
		}
	}
	r = msc_update_object(card, objectId, 0, buffer, bufferSize);
	free(buffer);
	if(r < 0) return r;
	
	
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x32, keyLocation, 0x00);
	apdu.lc = 6;
	apdu.data = apduBuffer;
	apdu.datalen = 6;
	p = apduBuffer;
	memcpy(p, &read, 2); p+=2;
	memcpy(p, &write, 2); p+=2;
	memcpy(p, &use, 2); p+=2;	
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, r, "APDU transmit failed");
	if(apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		msc_delete_object(card, objectId, 0);
		return 0;
	}
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r) {
		if (card->ctx->debug >= 2) {
			sc_debug(card->ctx, "keyimport: got strange SWs: 0x%02X 0x%02X\n",
			     apdu.sw1, apdu.sw2);
		}
		/* no error checks.. this is last ditch cleanup */
		msc_delete_object(card, objectId, 0);
		SC_FUNC_RETURN(card->ctx, 0, r);
	}
	/* no error checks.. this is last ditch cleanup */
	msc_delete_object(card, objectId, 0);

	SC_FUNC_RETURN(card->ctx, 0, SC_ERROR_CARD_CMD_FAILED);
}
#undef CPYVAL

/* For future implementation of check_sw */
/*
switch(apdu.sw1) {
	case 0x9C:
	switch(apdu.sw2) {
		case 0x03: // Operation not allowed
		case 0x05: // Unsupported
		case 0x06: // Unauthorized
		case 0x11: // Bad private key num
		case 0x12: // Bad public key num
		case 0x0E:
		case 0x0F: // Invalid parameters...
	}
}
*/