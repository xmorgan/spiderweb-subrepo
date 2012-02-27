/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Archive code.
 *
 * The Initial Developer of the Original Code is Google Inc.
 * Portions created by the Initial Developer are Copyright (C) 2005
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Darin Fisher <darin@meer.net>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "mar_private.h"
#include "mar.h"

#ifdef XP_WIN
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

/* this is the same hash algorithm used by nsZipArchive.cpp */
static PRUint32 mar_hash_name(const char *name) {
  PRUint32 val = 0;
  unsigned char* c;

  for (c = (unsigned char *) name; *c; ++c)
    val = val*37 + *c;

  return val % TABLESIZE;
}

static int mar_insert_item(MarFile *mar, const char *name, int namelen,
                           PRUint32 offset, PRUint32 length, PRUint32 flags) {
  MarItem *item, *root;
  PRUint32 hash;
  
  item = (MarItem *) malloc(sizeof(MarItem) + namelen);
  if (!item)
    return -1;
  item->next = NULL;
  item->offset = offset;
  item->length = length;
  item->flags = flags;
  memcpy(item->name, name, namelen + 1);

  hash = mar_hash_name(name);

  root = mar->item_table[hash];
  if (!root) {
    mar->item_table[hash] = item;
  } else {
    /* append item */
    while (root->next)
      root = root->next;
    root->next = item;
  }
  return 0;
}

static int mar_consume_index(MarFile *mar, char **buf, const char *buf_end) {
  /*
   * Each item has the following structure:
   *   PRUint32 offset      (network byte order)
   *   PRUint32 length      (network byte order)
   *   PRUint32 flags       (network byte order)
   *   char     name[N]     (where N >= 1)
   *   char     null_byte;
   */
  PRUint32 offset;
  PRUint32 length;
  PRUint32 flags;
  const char *name;
  int namelen;

  if ((buf_end - *buf) < (int)(3*sizeof(PRUint32) + 2))
    return -1;

  memcpy(&offset, *buf, sizeof(offset));
  *buf += sizeof(offset);

  memcpy(&length, *buf, sizeof(length));
  *buf += sizeof(length);

  memcpy(&flags, *buf, sizeof(flags));
  *buf += sizeof(flags);

  offset = ntohl(offset);
  length = ntohl(length);
  flags = ntohl(flags);

  name = *buf;
  /* find namelen; must take care not to read beyond buf_end */
  while (**buf) {
    if (*buf == buf_end)
      return -1;
    ++(*buf);
  }
  namelen = (*buf - name);
  /* consume null byte */
  if (*buf == buf_end)
    return -1;
  ++(*buf);

  return mar_insert_item(mar, name, namelen, offset, length, flags);
}

static int mar_read_index(MarFile *mar) {
  char id[MAR_ID_SIZE], *buf, *bufptr, *bufend;
  PRUint32 offset_to_index, size_of_index;

  /* verify MAR ID */
  if (fread(id, MAR_ID_SIZE, 1, mar->fp) != 1)
    return -1;
  if (memcmp(id, MAR_ID, MAR_ID_SIZE) != 0)
    return -1;

  if (fread(&offset_to_index, sizeof(PRUint32), 1, mar->fp) != 1)
    return -1;
  offset_to_index = ntohl(offset_to_index);

  if (fseek(mar->fp, offset_to_index, SEEK_SET))
    return -1;
  if (fread(&size_of_index, sizeof(PRUint32), 1, mar->fp) != 1)
    return -1;
  size_of_index = ntohl(size_of_index);

  buf = (char *) malloc(size_of_index);
  if (!buf)
    return -1;
  if (fread(buf, size_of_index, 1, mar->fp) != 1) {
    free(buf);
    return -1;
  }

  bufptr = buf;
  bufend = buf + size_of_index;
  while (bufptr < bufend && mar_consume_index(mar, &bufptr, bufend) == 0);

  free(buf);
  return (bufptr == bufend) ? 0 : -1;
}

/**
 * Internal shared code for mar_open and mar_wopen.
 * On failure, will fclose(fp).
 */
static MarFile *mar_fpopen(FILE *fp)
{
  MarFile *mar;

  mar = (MarFile *) malloc(sizeof(*mar));
  if (!mar) {
    fclose(fp);
    return NULL;
  }

  mar->fp = fp;
  memset(mar->item_table, 0, sizeof(mar->item_table));
  if (mar_read_index(mar)) {
    mar_close(mar);
    return NULL;
  }

  return mar;
}

MarFile *mar_open(const char *path) {
  FILE *fp;

  fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  return mar_fpopen(fp);
}

#ifdef XP_WIN
MarFile *mar_wopen(const PRUnichar *path) {
  FILE *fp;

  fp = _wfopen(path, L"rb");
  if (!fp)
    return NULL;

  return mar_fpopen(fp);
}
#endif

void mar_close(MarFile *mar) {
  MarItem *item;
  int i;

  fclose(mar->fp);

  for (i = 0; i < TABLESIZE; ++i) {
    item = mar->item_table[i];
    while (item) {
      MarItem *temp = item;
      item = item->next;
      free(temp);
    }
  }

  free(mar);
}

/** 
 * Reads the product info block from the MAR file's additional block section.
 * The caller is responsible for freeing the fields in infoBlock
 * if the return is successful.
 *
 * @param infoBlock Out parameter for where to store the result to
 * @return 0 on success, -1 on failure
*/
int
read_product_info_block(char *path, 
                        struct ProductInformationBlock *infoBlock)
{
  int rv;
  MarFile mar;
  mar.fp = fopen(path, "rb");
  if (!mar.fp) {
    return -1;
  }
  rv = mar_read_product_info_block(&mar, infoBlock);
  fclose(mar.fp);
  return rv;
}

/** 
 * Reads the product info block from the MAR file's additional block section.
 * The caller is responsible for freeing the fields in infoBlock
 * if the return is successful.
 *
 * @param infoBlock Out parameter for where to store the result to
 * @return 0 on success, -1 on failure
*/
int
mar_read_product_info_block(MarFile *mar, 
                            struct ProductInformationBlock *infoBlock)
{
  int i, hasAdditionalBlocks, offset, 
    offsetAdditionalBlocks, numAdditionalBlocks,
    additionalBlockSize, additionalBlockID;
  /* The buffer size is 97 bytes because the MAR channel name < 64 bytes, and 
     product version < 32 bytes + 3 NULL terminator bytes. */
  char buf[97] = { '\0' };
  int ret = get_mar_file_info_fp(mar->fp, NULL, NULL,
                                 &hasAdditionalBlocks, 
                                 &offsetAdditionalBlocks, 
                                 &numAdditionalBlocks);
  for (i = 0; i < numAdditionalBlocks; ++i) {
    /* Read the additional block size */
    if (fread(&additionalBlockSize, 
              sizeof(additionalBlockSize), 
              1, mar->fp) != 1) {
      return -1;
    }
    additionalBlockSize = ntohl(additionalBlockSize) - 
                          sizeof(additionalBlockSize) - 
                          sizeof(additionalBlockID);

    /* Read the additional block ID */
    if (fread(&additionalBlockID, 
              sizeof(additionalBlockID), 
              1, mar->fp) != 1) {
      return -1;
    }
    additionalBlockID = ntohl(additionalBlockID);

    if (PRODUCT_INFO_BLOCK_ID == additionalBlockID) {
      const char *location;
      int len;

      /* This block must be at most 104 bytes.
         MAR channel name < 64 bytes, and product version < 32 bytes + 3 NULL 
         terminator bytes. We only check for 96 though because we remove 8 
         bytes above from the additionalBlockSize: We subtract 
         sizeof(additionalBlockSize) and sizeof(additionalBlockID) */
      if (additionalBlockSize > 96) {
        return -1;
      }

    if (fread(buf, additionalBlockSize, 1, mar->fp) != 1) {
        return -1;
      }

      /* Extract the MAR channel name from the buffer.  For now we
         point to the stack allocated buffer but we strdup this
         if we are within bounds of each field's max length. */
      location = buf;
      len = strlen(location);
      infoBlock->MARChannelID = location;
      location += len + 1;
      if (len >= 64) {
        infoBlock->MARChannelID = NULL;
        return -1;
      }

      /* Extract the version from the buffer */
      len = strlen(location);
      infoBlock->productVersion = location;
      location += len + 1;
      if (len >= 32) {
        infoBlock->MARChannelID = NULL;
        infoBlock->productVersion = NULL;
        return -1;
      }
      infoBlock->MARChannelID = 
        strdup(infoBlock->MARChannelID);
      infoBlock->productVersion = 
        strdup(infoBlock->productVersion);
      return 0;
    } else {
      /* This is not the additional block you're looking for. Move along. */
      if (fseek(mar->fp, additionalBlockSize, SEEK_CUR)) {
        return -1;
      }
    }
  }

  /* If we had a product info block we would have already returned */
  return -1;
}

const MarItem *mar_find_item(MarFile *mar, const char *name) {
  PRUint32 hash;
  const MarItem *item;

  hash = mar_hash_name(name);

  item = mar->item_table[hash];
  while (item && strcmp(item->name, name) != 0)
    item = item->next;

  return item;
}

int mar_enum_items(MarFile *mar, MarItemCallback callback, void *closure) {
  MarItem *item;
  int i;

  for (i = 0; i < TABLESIZE; ++i) {
    item = mar->item_table[i];
    while (item) {
      int rv = callback(mar, item, closure);
      if (rv)
        return rv;
      item = item->next;
    }
  }

  return 0;
}

int mar_read(MarFile *mar, const MarItem *item, int offset, char *buf,
             int bufsize) {
  int nr;

  if (offset == (int) item->length)
    return 0;
  if (offset > (int) item->length)
    return -1;

  nr = item->length - offset;
  if (nr > bufsize)
    nr = bufsize;

  if (fseek(mar->fp, item->offset + offset, SEEK_SET))
    return -1;

  return fread(buf, 1, nr, mar->fp);
}


/**
 * Determines the MAR file information.
 *
 * @param path                   The path of the MAR file to check.
 * @param hasSignatureBlock      Optional out parameter specifying if the MAR
 *                               file is has a signature block or not.
 * @param numSignatures          Optional out parameter for storing the number
 *                               of signatures in the MAR file.
 * @param hasAdditionalBlocks    Optional out parameter specifying if the MAR
 *                               file has additional blocks or not.
 * @param offsetAdditionalBlocks Optional out parameter for the offset to the 
 *                               first additional block. Value is only valid if
 *                               has_additional_blocks
 *                               is not equal to 0.
 * @param numAdditionalBlocks    Optional out parameter for the number of
 *                               additional blocks.  Value is only valid if
 *                               has_additional_blocks is not euqal to 0.
 * @return 0 on success and non-zero on failure.
 */
int get_mar_file_info(const char *path, 
                      int *hasSignatureBlock,
                      int *numSignatures,
                      int *hasAdditionalBlocks,
                      int *offsetAdditionalBlocks,
                      int *numAdditionalBlocks)
{
  int rv;
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    return -1;
  }

  rv = get_mar_file_info_fp(fp, hasSignatureBlock, 
                            numSignatures, hasAdditionalBlocks,
                            offsetAdditionalBlocks, numAdditionalBlocks);

  fclose(fp);
  return rv;
}

/**
 * Determines the MAR file information.
 *
 * @param fp                     An opened MAR file in read mode.
 * @param hasSignatureBlock      Optional out parameter specifying if the MAR
 *                               file is has a signature block or not.
 * @param numSignatures          Optional out parameter for storing the number
 *                               of signatures in the MAR file.
 * @param hasAdditionalBlocks    Optional out parameter specifying if the MAR
 *                               file has additional blocks or not.
 * @param offsetAdditionalBlocks Optional out parameter for the offset to the 
 *                               first additional block. Value is only valid if
 *                               has_additional_blocks
 *                               is not equal to 0.
 * @param numAdditionalBlocks    Optional out parameter for the number of
 *                               additional blocks.  Value is only valid if
 *                               has_additional_blocks is not euqal to 0.
 * @return 0 on success and non-zero on failure.
 */
int get_mar_file_info_fp(FILE *fp, 
                         int *hasSignatureBlock,
                         int *numSignatures,
                         int *hasAdditionalBlocks,
                         int *offsetAdditionalBlocks,
                         int *numAdditionalBlocks)
{
  PRUint32 offsetToIndex, offsetToContent, signatureCount, signatureLen;
  int i;
  
  /* One of hasSignatureBlock or hasAdditionalBlocks must be non NULL */
  if (!hasSignatureBlock && !hasAdditionalBlocks) {
    return -1;
  }


  /* Skip to the start of the offset index */
  if (fseek(fp, MAR_ID_SIZE, SEEK_SET)) {
    return -1;
  }

  /* Read the offset to the index. */
  if (fread(&offsetToIndex, sizeof(offsetToIndex), 1, fp) != 1) {
    return -1;
  }
  offsetToIndex = ntohl(offsetToIndex);

  if (numSignatures) {
     /* Skip past the MAR file size field */
    if (fseek(fp, sizeof(PRUint64), SEEK_CUR)) {
      return -1;
    }

    /* Read the offset to the index. */
    if (fread(numSignatures, sizeof(*numSignatures), 1, fp) != 1) {
      return -1;
    }
    *numSignatures = ntohl(*numSignatures);
  }

  /* Skip to the first index entry past the index size field 
     We do it in 2 calls because offsetToIndex + sizeof(PRUint32) 
     could oerflow in theory. */
  if (fseek(fp, offsetToIndex, SEEK_SET)) {
    return -1;
  }

  if (fseek(fp, sizeof(PRUint32), SEEK_CUR)) {
    return -1;
  }

  /* Read the first offset to content field. */
  if (fread(&offsetToContent, sizeof(offsetToContent), 1, fp) != 1) {
    return -1;
  }
  offsetToContent = ntohl(offsetToContent);

  /* Check if we have a new or old MAR file */
  if (hasSignatureBlock) {
    if (offsetToContent == MAR_ID_SIZE + sizeof(PRUint32)) {
      *hasSignatureBlock = 0;
    } else {
      *hasSignatureBlock = 1;
    }
  }

  /* If the caller doesn't care about the product info block 
     value, then just return */
  if (!hasAdditionalBlocks) {
    return 0;
  }

   /* Skip to the start of the signature block */
  if (fseeko(fp, SIGNATURE_BLOCK_OFFSET, SEEK_SET)) {
    return -1;
  }

  /* Get the number of signatures */
  if (fread(&signatureCount, sizeof(signatureCount), 1, fp) != 1) {
    return -1;
  }
  signatureCount = ntohl(signatureCount);

  /* Check that we have less than the max amount of signatures so we don't
     waste too much of either updater's or signmar's time. */
  if (signatureCount > MAX_SIGNATURES) {
    return -1;
  }

  /* Skip past the whole signature block */
  for (i = 0; i < signatureCount; i++) {
    /* Skip past the signature algorithm ID */
    if (fseek(fp, sizeof(PRUint32), SEEK_CUR)) {
      return -1;
    }

    /* Read the signature length and skip past the signature */
    if (fread(&signatureLen, sizeof(PRUint32), 1, fp) != 1) {
      return -1;
    }
    signatureLen = ntohl(signatureLen);
    if (fseek(fp, signatureLen, SEEK_CUR)) {
      return -1;
    }
  }

  if (ftell(fp) == offsetToContent) {
    *hasAdditionalBlocks = 0;
  } else {
    if (numAdditionalBlocks) {
      /* We have an additional block, so read in the number of additional blocks
         and set the offset. */
      *hasAdditionalBlocks = 1;
      if (fread(numAdditionalBlocks, sizeof(PRUint32), 1, fp) != 1) {
        return -1;
      }
      *numAdditionalBlocks = ntohl(*numAdditionalBlocks);
      if (offsetAdditionalBlocks) {
        *offsetAdditionalBlocks = ftell(fp);
      }
    } else if (offsetAdditionalBlocks) {
      /* numAdditionalBlocks is not specified but offsetAdditionalBlocks 
         is, so fill it! */
      *offsetAdditionalBlocks = ftell(fp) + sizeof(PRUint32);
    }
  }

  return 0;
}
