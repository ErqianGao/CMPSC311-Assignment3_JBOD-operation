#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#include "mdadm.h"
#include "jbod.h"

bool unmounted = true;

void encode_operation( uint32_t *op, jbod_cmd_t cmd, int disk_num, int block_num ){
  uint32_t temp = 0;
  *op = temp | cmd << 26 | disk_num << 22 | block_num;
}

void translate_address( uint32_t linear_address, int *disk_num, int *block_num, int *offset ){
  *disk_num = linear_address / JBOD_DISK_SIZE;
  *block_num = ( linear_address % JBOD_DISK_SIZE ) / JBOD_BLOCK_SIZE;
  *offset = ( linear_address % JBOD_DISK_SIZE ) % JBOD_BLOCK_SIZE;
}

void readOverBlock( int disk_num, int block_num, int offset, int len, uint8_t *buf ){
  uint8_t *temp;
  uint32_t op;
  //this part is for the read in the first block
  temp = malloc( JBOD_BLOCK_SIZE );
      
  encode_operation( &op, JBOD_SEEK_TO_DISK, disk_num, 0 );
  jbod_operation( op, NULL );
    
  encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
  jbod_operation( op, NULL );

  encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
  jbod_operation( op, temp );

  int copiedLen = JBOD_BLOCK_SIZE - offset;
  if( len < JBOD_BLOCK_SIZE && offset == 0 ){
    copiedLen = len;
  }
  memcpy( buf, temp + offset, copiedLen );
  free( temp );

  block_num++;

  int position = len - copiedLen;
  
  while( position > JBOD_BLOCK_SIZE ){
    temp = malloc( JBOD_BLOCK_SIZE );
  
    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
      
    encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
    jbod_operation( op, temp );

    memcpy( buf + copiedLen, temp, JBOD_BLOCK_SIZE );

    copiedLen = copiedLen + JBOD_BLOCK_SIZE;
    position = position - JBOD_BLOCK_SIZE;
    block_num++;
    free( temp );
    }

  if( position > 0 ){
    temp = malloc( JBOD_BLOCK_SIZE );
  
    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
      
    encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
    jbod_operation( op, temp );
  
    memcpy( buf + copiedLen, temp, position );

    free( temp );
  }
}


void writeOverBlock(  int disk_num, int block_num, int offset, int len, const uint8_t *buf ){
  uint8_t *temp;
  uint32_t op;

  temp = malloc( JBOD_BLOCK_SIZE );
    
  encode_operation( &op, JBOD_SEEK_TO_DISK, disk_num, 0 );
  jbod_operation( op, NULL );
    
  encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
  jbod_operation( op, NULL );
    
  encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
  jbod_operation( op, temp );
  int copiedLen = JBOD_BLOCK_SIZE - offset;
    
  memcpy( temp + offset, buf, copiedLen );

  encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
  jbod_operation( op, NULL );
    
  encode_operation( &op, JBOD_WRITE_BLOCK, 0, 0 );
  jbod_operation( op, temp );
    
  free( temp );
  block_num++;

  int leftLen = len - copiedLen;

  if( len < JBOD_BLOCK_SIZE && offset == 0 ){
    copiedLen = len;
  }
  
  while( leftLen > JBOD_BLOCK_SIZE ){
    temp = malloc( JBOD_BLOCK_SIZE );
    memcpy( temp, buf + copiedLen, JBOD_BLOCK_SIZE );

    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );

    encode_operation( &op, JBOD_WRITE_BLOCK, 0, 0 );
    jbod_operation( op, temp );
    
    copiedLen += JBOD_BLOCK_SIZE;
    leftLen -= JBOD_BLOCK_SIZE;
    block_num++;
    free( temp );
  }

  if( leftLen > 0 ){
    temp = malloc( JBOD_BLOCK_SIZE );

    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
    jbod_operation( op, temp );
    
    memcpy( temp, buf + copiedLen, leftLen );

    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );

    encode_operation( &op, JBOD_WRITE_BLOCK, 0, 0 );
    jbod_operation( op, temp );

    free( temp );
  }
  
}


int mdadm_mount(void) {
  uint32_t mountOp = 0;
  encode_operation( &mountOp, JBOD_MOUNT, 0, 0 );
  if( jbod_operation(mountOp, NULL) == 0 && unmounted == true )
    {
      unmounted = false;
      return 1;
    }
  return -1;
}


int mdadm_unmount(void) {
  uint32_t unmountOp = 0;
  encode_operation( &unmountOp, JBOD_UNMOUNT, 0, 0 );
  if( jbod_operation(unmountOp, NULL) == 0 && unmounted == false )
    {
      unmounted = true;
      return 1;
    }
  return -1;
}

int mdadm_read(uint32_t addr, uint32_t len, uint8_t *buf) {
  if( unmounted == true || len > 1024 || addr + len  >= JBOD_DISK_SIZE*JBOD_NUM_DISKS || ( buf == NULL && len != 0 ) ){
      return -1;
  }

  int disk_num = 0, block_num = 0, offset = 0;
  translate_address( addr, &disk_num, &block_num, &offset );


  
  if( addr%JBOD_DISK_SIZE + len > JBOD_DISK_SIZE ){ //read over disk
    int firstHalf = JBOD_DISK_SIZE - ( addr%JBOD_DISK_SIZE );
    readOverBlock( disk_num, block_num, offset, firstHalf, buf );
    readOverBlock( disk_num + 1, 0, 0, len - firstHalf, buf + firstHalf );

    return len;
  }
  else if( offset + len > JBOD_BLOCK_SIZE ){ //read over block
    readOverBlock( disk_num, block_num, offset, len, buf );

    return len;
  }
  else{
    uint8_t *temp;
    uint32_t op;
    temp = malloc( JBOD_BLOCK_SIZE );
    
    encode_operation( &op, JBOD_SEEK_TO_DISK, disk_num, 0 );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
    jbod_operation( op, temp );
    
    memcpy( buf, temp + offset, len );
    free( temp );
  }
  return len;
}



int mdadm_write(uint32_t addr, uint32_t len, const uint8_t *buf) {
  if( unmounted == true || len > 1024 || addr + len > JBOD_DISK_SIZE*JBOD_NUM_DISKS || addr < 0 || ( buf == NULL && len != 0 ) ){
      return -1;
  }

  int disk_num = 0, block_num = 0, offset = 0;
  translate_address( addr, &disk_num, &block_num, &offset );

  
  if( addr%JBOD_DISK_SIZE + len > JBOD_DISK_SIZE ){ //write over disk
    int firstHalf = JBOD_DISK_SIZE - ( addr%JBOD_DISK_SIZE );
    writeOverBlock( disk_num, block_num, offset, firstHalf, buf );
    writeOverBlock( disk_num + 1, 0, 0, len - firstHalf, buf + firstHalf );

    return len;
  }
  else if( offset + len > JBOD_BLOCK_SIZE ){ //write over block
    writeOverBlock( disk_num, block_num, offset, len, buf );

    return len;
  }
  else{
    uint8_t *temp;
    uint32_t op;
    temp = malloc( JBOD_BLOCK_SIZE );
    
    encode_operation( &op, JBOD_SEEK_TO_DISK, disk_num, 0 );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_READ_BLOCK, 0, 0 );
    jbod_operation( op, temp );
    
    memcpy( temp + offset, buf, len );

    encode_operation( &op, JBOD_SEEK_TO_BLOCK, 0, block_num );
    jbod_operation( op, NULL );
    
    encode_operation( &op, JBOD_WRITE_BLOCK, 0, 0 );
    jbod_operation( op, temp );
    
    free( temp );
  }
  
  return len;
}



