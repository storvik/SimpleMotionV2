#include "devicedeployment.h"
#include "user_options.h"
#include "crc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <simplemotion_private.h>
#include <math.h>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
void sleep_ms(int millisecs)
{
    usleep(millisecs*1000);
}
#else
#include <windows.h>
void sleep_ms(int millisecs)
{
    Sleep(millisecs);
}
#endif

int globalErrorDetailCode=0;

smbool loadBinaryFile( const char *filename, smuint8 **data, int *numbytes );

int smGetDeploymentToolErrroDetail()
{
    return globalErrorDetailCode;
}

//return -1 if EOF
//readPosition should be initialized to 0 and not touched by caller after that. this func will increment it after each call.
unsigned int readFileLine( const smuint8 *data, const int dataLen, int *readPosition, int charlimit, char *output, smbool *eof)
{
    int len=0;
    char c;
    do
    {
        if((*readPosition)>=dataLen)//end of data buffer
        {
            *eof=smtrue;
            c=0;
        }
        else
        {
            *eof=smfalse;
            c=data[(*readPosition)];
            (*readPosition)++;
        }

        //eol or eof
        if( (*eof)==smtrue || c=='\n' || c=='\r' || len>=charlimit-1 )
        {
            output[len]=0;//terminate str
            return len;
        }

        output[len]=c;
        len++;
    } while(1);
    return len;
}

typedef struct
{
    int address;
    double value;
    smbool readOnly;
    double scale;
    double offset;
} Parameter;

#define maxLineLen 100

smbool parseParameter( const smuint8 *drcData, const int drcDataLen, int idx, Parameter *param )
{
    char line[maxLineLen];
    char scanline[maxLineLen];
    smbool gotaddr=smfalse,gotvalue=smfalse, gotreadonly=smfalse, gotscale=smfalse,gotoffset=smfalse;
    unsigned int readbytes;
    int readPosition=0;
    smbool eof;

    do//loop trhu all lines of file
    {
        readbytes=readFileLine(drcData,drcDataLen,&readPosition,maxLineLen,line,&eof);//read line

        //try read address
        sprintf(scanline,"%d\\addr=",idx);
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes > strlen(scanline))//string starts with correct line
            if(sscanf(line+strlen(scanline),"%d",&param->address)==1)//parse number after the start of line
                gotaddr=smtrue;//number parse success

        //try read value
        sprintf(scanline,"%d\\value=",idx);
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes > strlen(scanline))//string starts with correct line
            if(sscanf(line+strlen(scanline),"%lf",&param->value)==1)//parse number after the start of line
                gotvalue=smtrue;//number parse success

        //try read offset
        sprintf(scanline,"%d\\offset=",idx);
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes > strlen(scanline))//string starts with correct line
            if(sscanf(line+strlen(scanline),"%lf",&param->offset)==1)//parse number after the start of line
                gotoffset=smtrue;//number parse success

        //try read scale
        sprintf(scanline,"%d\\scaling=",idx);
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes > strlen(scanline))//string starts with correct line
            if(sscanf(line+strlen(scanline),"%lf",&param->scale)==1)//parse number after the start of line
                gotscale=smtrue;//number parse success

        //try read readonly status
        sprintf(scanline,"%d\\readonly=true",idx);//check if readonly=true
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes >= strlen(scanline))//line match
        {
            param->readOnly=smtrue;
            gotreadonly=smtrue;
        }
        sprintf(scanline,"%d\\readonly=false",idx);//check if readonly=false
        if(!strncmp(line,scanline,strlen(scanline)) && readbytes >= strlen(scanline))//line match
        {
            param->readOnly=smfalse;
            gotreadonly=smtrue;
        }
    }
    while( (gotvalue==smfalse || gotaddr==smfalse || gotreadonly==smfalse || gotscale==smfalse || gotoffset==smfalse) && eof==smfalse );

    if(gotvalue==smtrue&&gotaddr==smtrue&&gotoffset==smtrue&&gotscale==smtrue&&gotreadonly==smtrue)
    {
        return smtrue;
    }

    return smfalse;//not found
}

/**
 * @brief smConfigureParameters Configures all target device parameters from file and performs device restart if necessary. This can take few seconds to complete. This may take 2-5 seconds to call.
 * @param smhandle SM bus handle, must be opened before call
 * @param smaddress Target SM device address
 * @param filename .DRC file name
 * @param mode Combined from CONFIGMODE_ define bits (can logic OR mutliple values).
 * @return Enum LoadConfigurationStatus
 *
 * Requires DRC file version 111 or later to use CONFIGMODE_REQUIRE_SAME_FW.
 */
LoadConfigurationStatus smLoadConfiguration(const smbus smhandle, const int smaddress, const char *filename, unsigned int mode , int *skippedCount, int *errorCount)
{
    LoadConfigurationStatus ret;
    smuint8 *drcData=NULL;
    int drcDataLength;

    if(loadBinaryFile(filename,&drcData,&drcDataLength)!=smtrue)
        return CFGUnableToOpenFile;

    ret = smLoadConfigurationFromBuffer( smhandle, smaddress, drcData, drcDataLength, mode, skippedCount, errorCount );
    free(drcData);

    return ret;
}

/**
 * @brief smConfigureParametersFromBuffer Same as smConfigureParameters but reads data from user specified memory address instead of file. Configures all target device parameters from file and performs device restart if necessary. This can take few seconds to complete. This may take 2-5 seconds to call.
 * @param smhandle SM bus handle, must be opened before call
 * @param smaddress Target SM device address
 * @param drcData Pointer to to a memory where .drc file is loaded
 * @param drcDataLen Number of bytes available in the drcData buffer
 * @param mode Combined from CONFIGMODE_ define bits (can logic OR mutliple values).
 * @return Enum LoadConfigurationStatus
 *
 * Requires DRC file version 111 or later to use CONFIGMODE_REQUIRE_SAME_FW.
 */
LIB LoadConfigurationStatus smLoadConfigurationFromBuffer( const smbus smhandle, const int smaddress, const smuint8 *drcData, const int drcDataLength, unsigned int mode, int *skippedCount, int *errorCount )
{
    //test connection
    smint32 devicetype;
    SM_STATUS stat;
    int ignoredCount=0;
    int setErrors=0;
    smint32 CB1Value;
    int changed=0;
    *skippedCount=-1;
    *errorCount=-1;

    //test connection
    resetCumulativeStatus(smhandle);
    stat=smRead1Parameter(smhandle,smaddress,SMP_DEVICE_TYPE,&devicetype);
    if(stat!=SM_OK)
        return CFGCommunicationError;

    //smSetParameter( smhandle, smaddress, SMP_RETURN_PARAM_LEN, SMPRET_CMD_STATUS );//get command status as feedback from each executed SM command

    if(mode&CONFIGMODE_DISABLE_DURING_CONFIG)
    {
        smRead1Parameter( smhandle, smaddress, SMP_CONTROL_BITS1, &CB1Value );
        smSetParameter( smhandle, smaddress, SMP_CONTROL_BITS1, 0);//disable drive
    }

    if(getCumulativeStatus( smhandle )!=SM_OK )
        return CFGCommunicationError;

    smDebug(smhandle,SMDebugLow,"Setting parameters\n");

    int i=1;
    smbool readOk;
    do
    {
        Parameter param;
        readOk=parseParameter(drcData,drcDataLength,i,&param);

        if(readOk==smtrue && param.readOnly==smfalse)
        {
            smint32 currentValue;

            int configFileValue=round(param.value*param.scale-param.offset);

            //set parameter to device
            if(smRead1Parameter( smhandle, smaddress, param.address, &currentValue )==SM_OK)
            {
                if(currentValue!=configFileValue  ) //set only if different
                {
                    resetCumulativeStatus( smhandle );
                    smint32 dummy;
                    smint32 cmdSetAddressStatus;
                    smint32 cmdSetValueStatus;

                    //use low level SM commands so we can get execution status of each subpacet:
                    smAppendSMCommandToQueue( smhandle, SM_SET_WRITE_ADDRESS, SMP_RETURN_PARAM_LEN );
                    smAppendSMCommandToQueue( smhandle, SM_WRITE_VALUE_24B, SMPRET_CMD_STATUS );
                    smAppendSMCommandToQueue( smhandle, SM_SET_WRITE_ADDRESS, param.address );
                    smAppendSMCommandToQueue( smhandle, SM_WRITE_VALUE_32B, configFileValue );
                    smExecuteCommandQueue( smhandle, smaddress );
                    smGetQueuedSMCommandReturnValue( smhandle, &dummy );
                    smGetQueuedSMCommandReturnValue( smhandle, &dummy );
                    smGetQueuedSMCommandReturnValue( smhandle, &cmdSetAddressStatus );
                    smGetQueuedSMCommandReturnValue( smhandle, &cmdSetValueStatus );

                    //check if above code succeed
                    if( getCumulativeStatus(smhandle)!=SM_OK || cmdSetAddressStatus!=SMP_CMD_STATUS_ACK || cmdSetValueStatus!=SMP_CMD_STATUS_ACK )
                    {
                        SM_STATUS stat=getCumulativeStatus(smhandle);
                        setErrors++;
                        smDebug(smhandle,SMDebugLow,"Failed to write parameter value %d to address %d (status: %d %d %d)\n",configFileValue,param.address,(int)stat,cmdSetAddressStatus,cmdSetValueStatus);
                    }

                    changed++;
                }
            }
            else//device doesn't have such parameter. perhaps wrong model or fw version.
            {
                ignoredCount++;
                smDebug(smhandle,SMDebugLow,"Ignoring parameter parameter value %d to address %d\n",configFileValue,param.address);
            }
        }

        i++;
    } while(readOk==smtrue);

    *skippedCount=ignoredCount;
    *errorCount=setErrors;

    resetCumulativeStatus( smhandle );

    //save to flash if some value was changed
    if(changed>0)
        smSetParameter( smhandle, smaddress, SMP_SYSTEM_CONTROL, SMP_SYSTEM_CONTROL_SAVECFG );

    if(mode&CONFIGMODE_CLEAR_FAULTS_AFTER_CONFIG )
    {
        smDebug(smhandle,SMDebugLow,"Restting faults\n");
        smSetParameter( smhandle, smaddress, SMP_FAULTS, 0 );//reset faults
    }

    //re-enable drive
    if(mode&CONFIGMODE_DISABLE_DURING_CONFIG)
    {
        smDebug(smhandle,SMDebugLow,"Restoring CONTROL_BITS1 to value 0x%x\n",CB1Value);
        smSetParameter( smhandle, smaddress, SMP_CONTROL_BITS1, CB1Value );//restore controbits1 (enable if it was enabled before)
    }

    smint32 statusbits;
    smRead1Parameter( smhandle, smaddress, SMP_STATUS, &statusbits );

    //restart drive if necessary or if forced
    if( (statusbits&STAT_PERMANENT_STOP) || (mode&CONFIGMODE_ALWAYS_RESTART_TARGET) )
    {
        smDebug(smhandle,SMDebugLow,"Restarting device\n");
        smSetParameter( smhandle, smaddress, SMP_SYSTEM_CONTROL, SMP_SYSTEM_CONTROL_RESTART );
        sleep_ms(2000);//wait power-on
    }

    if(getCumulativeStatus(smhandle)!=SM_OK)
        return CFGCommunicationError;

    return CFGComplete;
}

/**
 * @brief smGetDeviceFirmwareUniqueID Reads installed firmware binary checksum that can be used to verify whether a wanted FW version is installed
 * @param smhandle SM bus handle, must be opened before call
 * @param smaddress Target SM device address. Can be device in DFU mode or main operating mode. For Argon, one device in a bus must be started into DFU mode by DIP switches and smaddress must be set to 255.
 * @param UID result will be written to this pointer
 * @return smtrue if success, smfalse if failed (if communication otherwise works, then probably UID feature not present in this firmware version)
 */
smbool smGetDeviceFirmwareUniqueID( smbus smhandle, int deviceaddress, smuint32 *UID )//FIXME gives questionable value if device is in DFU mode. Check FW side.
{
    smint32 fwBinaryChecksum;
    resetCumulativeStatus(smhandle);
    smSetParameter( smhandle, deviceaddress, SMP_SYSTEM_CONTROL, SMP_SYSTEM_CONTROL_GET_SPECIAL_DATA);
    smRead1Parameter( smhandle, deviceaddress, SMP_DEBUGPARAM1, &fwBinaryChecksum );
    *UID=(smuint32) fwBinaryChecksum;

    if(getCumulativeStatus(smhandle)==SM_OK)
        return smtrue;
    else
        return smfalse;
}

/* local helper functions to get 8, 16 or 32 bit value from data buffer.
 * these will increment *buf pointer on each call.
 *
 * FYI: motivation of using these, is that simple pointer cast to various sizes
 * might not work on all CPU architectures due to data alignment restrictions.
*/


smuint8 bufferGet8( smuint8 **buf )
{
    smuint8 ret=(*buf)[0];
    *buf++;
    return ret;
}

smuint16 bufferGet16( smuint8 **buf )
{
    UnionOf4Bytes d;
    d.U8[0]=(*buf)[0];
    d.U8[1]=(*buf)[1];

    smuint16 ret=d.U16[0];

    *buf+=2;
    return ret;
}

smuint32 bufferGet32( smuint8 **buf )
{
    UnionOf4Bytes d;
    d.U8[0]=(*buf)[0];
    d.U8[1]=(*buf)[1];
    d.U8[2]=(*buf)[2];
    d.U8[3]=(*buf)[3];

    smuint32 ret=d.U32;

    *buf+=4;
    return ret;
}


FirmwareUploadStatus verifyFirmwareData(smuint8 *data, smuint32 numbytes, smuint32 connectedDeviceTypeId,
                                        smuint32 *primaryMCUDataOffset, smuint32 *primaryMCUDataLenth,
                                        smuint32 *secondaryMCUDataOffset,smuint32 *secondaryMCUDataLength )
{
    //see https://granitedevices.com/wiki/Firmware_file_format_(.gdf)

    smuint32 filetype;
    filetype=((smuint32*)data)[0];
    if(filetype!=0x57464447) //check header string "GDFW"
        return FWInvalidFile;

    smuint32 filever, deviceid;

    filever=((smuint16*)data)[2];

    if(filever==300)//handle GDF version 300 here
    {
        smuint32 primaryMCUSize, secondaryMCUSize;

        deviceid=((smuint16*)data)[3];
        primaryMCUSize=((smuint32*)data)[2];
        secondaryMCUSize=((smuint32*)data)[3];
        if(secondaryMCUSize==0xffffffff)
            secondaryMCUSize=0;//it is not present

        if(deviceid/1000!=connectedDeviceTypeId/1000)//compare only device and model family. AABBCC so AAB is compared value, ARGON=004 IONI=011
            return FWIncompatibleFW;

        //get checksum and check it
        smuint32 cksum,cksumcalc=0;
        smuint32 i;
        smuint32 cksumOffset=4+2+2+4+4+primaryMCUSize+secondaryMCUSize;
        if(cksumOffset>numbytes-4)
            return FWInvalidFile;
        cksum=((smuint32*)(data+cksumOffset))[0];

        for(i=0;i< numbytes-4;i++)
        {
            cksumcalc+=data[i];
        }

        if(cksum!=cksumcalc)
            return FWIncompatibleFW;

        //let caller know where the firmware data is located in buffer
        *primaryMCUDataOffset=4+2+2+4+4;
        *primaryMCUDataLenth=primaryMCUSize;
        *secondaryMCUDataOffset=*primaryMCUDataOffset+*primaryMCUDataLenth;
        *secondaryMCUDataLength=secondaryMCUSize;
    }
    else if(filever>=400 && filever<500)//handle GDF versions 400-499 here
    {
        /* GDF version 400 format
         * ----------------------
         *
         * Note: GDF v400 is not limited to firmware files any more,
         * it can be used as general purpose data container for up to 4GB data chunks.
         *
         * Binary file contents
         * --------------------
         *
         * bytes  meaning:
         *
         * 4	ASCII string = "GDFW"
         * 2	GDF version = 400
         * 2	GDF backwards compatible version = 400
         * 4	File category = 100 for firmware files  (value range <2^31 is reserved and managed by GD, range from 2^31 to 2^32-1 are free for use by anyone for any purpose)
         * 4	number of data chunks in file = N
         *
         * repeat N times:
         * 4	data chunk descriptive name string length in bytes = L
         * L	data chunk descriptive name string in UTF8
         * 4	data chunk type
         * 4	data chunk option bits
         * 4	data chunk size in bytes=S
         * S	data
         * end of repeat
         *
         * 4	file's CRC-32
         *
         * data chunk types
         * ----------------
         * 0=target device name, UTF8
         * 1=firmware name, UTF8
         * 2=firmware version string, UTF8
         * 3=remarks, UTF8
         * 4=manufacturer, UTF8
         * 5=copyright, UTF8
         * 6=license, UTF8
         * 7=disclaimer, UTF8
         * 8=circulation, UTF8 (i.e. customer name)
         * 20=unix timestamp divided by 4, S=4
         * 50=target device type ID range, S=8
         *   first 4 bytes are lowest supported target device ID, i.e. 11000=IONI
         *   second 4 bytes are highest supported target device ID's, i.e. 11200=IONI PRO HC
         * 100=main MCU FW binary, S=any
         * 101=main MCU FW unique identifier number, S=4
         * 102=main MCU FW required HW feature bits, S=4
         *     helps to determine whether FW works on target
         *     device version when compared to a value readable from the device.
         *     0 means no requirements, works on all target ID devices.
         * 200=secondary MCU FW binary, S=any
         *
         * note: firmware may contain many combinations of above chunks. in basic case, it contains just chunk type 100 and nothing else.
         *
         * data chunk option bits
         * ----------------------
         * bit 0: if 1, GDF loading application must support/understand the chunk type to use this file
         * bits 1-31: reserved
         *
         */
        smuint8 *dataPtr=&data[6];//start at byte 6 because first 6 bytes are already read
        smuint8 *dataCRCPtr=&data[numbytes-4];
        smuint32 numOfChunks;
        smuint32 deviceid_max;
        smuint32 chunkType;
        smuint32 chunkTypeStringLen;
        smuint32 chunkSize;
        smuint16 chunkOptions;
        smuint32 i;
        smuint32 primaryMCU_FW_UID;

        //check file compatibility
        if(bufferGet16(&dataPtr)!=400)
            return FWIncompatibleFW;//this version of library requires file that is backwards compatible with version 400

        //check file category (100=firmware file)
        if(bufferGet32(&dataPtr)!=100)
            return FWInvalidFile;//not FW GDF file

        //check file CRC
        crcInit();
        crc calculatedCRC=crcFast((const unsigned char*)data, numbytes-4);
        crc fileCRC=bufferGet32(&dataCRCPtr);
        if(calculatedCRC!=fileCRC)
            return FWInvalidFile;//CRC mismatch

        //read data chunks
        numOfChunks=bufferGet32(&dataPtr);
        for( i=0; i<numOfChunks; i++ )
        {
            chunkTypeStringLen=bufferGet32(&dataPtr);
            dataPtr+=chunkTypeStringLen;//skip type string, we don't use it for now
            chunkType=bufferGet32(&dataPtr);
            chunkOptions=bufferGet32(&dataPtr);
            chunkSize=bufferGet32(&dataPtr);

            //handle chunk
            if(chunkType==50 && chunkSize==8)
            {
                //check target device type
                deviceid=bufferGet32(&dataPtr);
                deviceid_max=bufferGet32(&dataPtr);

                if(connectedDeviceTypeId<deviceid || connectedDeviceTypeId>deviceid_max)
                    return FWUnsupportedTargetDevice;
            }
            else if(chunkType==100)//main MCU FW
            {
                *primaryMCUDataOffset=(smuint32)(dataPtr-data);
                *primaryMCUDataLenth=chunkSize;
                dataPtr+=chunkSize;//skip to next chunk
            }
            else if(chunkType==200)//secondary MCU FW
            {
                *secondaryMCUDataOffset=(smuint32)(dataPtr-data);
                *secondaryMCUDataLength=chunkSize;
                dataPtr+=chunkSize;//skip to next chunk
            }
            else if(chunkType==101 && chunkSize==4)//main MCU FW unique identifier
            {
                primaryMCU_FW_UID=bufferGet32(&dataPtr);
            }
            else if(chunkOptions&1) //bit nr 0 is 1, which means we should be able to handle this chunk to support the GDF file, so as we don't know what chunk it is, this is an error
            {
                return FWIncompatibleFW;
            }
            else //unsupported chunk that we can skip
            {
                dataPtr+=chunkSize;//skip to next chunk
            }
        }

        if((smuint32)(dataPtr-data)!=numbytes-4)//check if chunks total size match file size
            return FWInvalidFile;
    }
    else
        return FWIncompatibleFW;//unsupported file version

    return FWComplete;
}

/**
 * @brief smFirmwareUploadStatusToString converts FirmwareUploadStatus enum to string.
 * @param string user supplied pointer where string will be stored. must have writable space for at least 100 characters.
 * @return void
 */
void smFirmwareUploadStatusToString(const FirmwareUploadStatus FWUploadStatus, char *string )
{
    int i;
    const int count=sizeof(FirmwareUploadStatusToString)/sizeof(FirmwareUploadStatusToStringType);

    for(i=0;i<count;i++)
    {
        if(FirmwareUploadStatusToString[i].FWUSEnum==FWUploadStatus)
        {
            strcpy(string,FirmwareUploadStatusToString[i].string);
            return;
        }
    }

    if((int)FWUploadStatus>=0 && (int)FWUploadStatus<=99 )
    {
        snprintf( string, 100, "Firmware install %d%% done", (int)FWUploadStatus);
        return;
    }

    snprintf( string, 100, "SimpleMotion lib error: unknown FW uload state (%d), please check input or report a bug", (int)FWUploadStatus);
}


smbool loadBinaryFile( const char *filename, smuint8 **data, int *numbytes )
{
    FILE *f;
    f=fopen(filename,"rb");
    if(f==NULL)
        return smfalse;

    *numbytes=0;

    //get length
    fseek(f,0,SEEK_END);
    int length=ftell(f);
    fseek(f,0,SEEK_SET);

    //allocate buffer
    *data=malloc(length);
    if(*data==NULL)
    {
        fclose(f);
        return smfalse;
    }

    //read
    *numbytes=fread(*data,1,length,f);
    if(*numbytes!=length)//failed to read it all
    {
        free(*data);
        *numbytes=0;
        fclose(f);
        return smfalse;
    }

    fclose(f);
    return smtrue;//successl
}



//flashing STM32 (host side mcu)
smbool flashFirmwarePrimaryMCU( smbus smhandle, int deviceaddress, const smuint8 *data, smint32 size, int *progress )
{
    smint32 ret;
    static smint32 deviceType, fwVersion;
    static int uploadIndex;
    int c;
    const int BL_CHUNK_LEN=32;
    static enum {Init,Upload,Finish} state=Init;

    if(state==Init)
    {
        resetCumulativeStatus( smhandle );
        smRead2Parameters( smhandle, deviceaddress, SMP_FIRMWARE_VERSION, &fwVersion, SMP_DEVICE_TYPE,&deviceType );

        if(getCumulativeStatus(smhandle)!=SM_OK)
        {
            state=Init;
            return smfalse;
        }

/*      kommentoitu pois koska ei haluta erasoida parskuja koska parametri SMO ei saisi nollautua mielellään

        if(deviceType!=4000)//argon does not support BL function 11
            smSetParameter(smhandle,deviceaddress,SMP_BOOTLOADER_FUNCTION,11);//BL func on ioni 11 = do mass erase on STM32, also confifuration
        else//does not reset on ioni and drives that support preserving settings. but resets on argon
*/
            smSetParameter(smhandle,deviceaddress,SMP_BOOTLOADER_FUNCTION,1);//BL func 1 = do mass erase on STM32. On Non-Argon devices it doesn't reset confifuration

        sleep_ms(2000);//wait some time. 500ms too little 800ms barely enough

        //flash
        smSetParameter(smhandle,deviceaddress,SMP_RETURN_PARAM_LEN, SMPRET_CMD_STATUS);

        if(getCumulativeStatus(smhandle)!=SM_OK)
        {
            state=Init;
            return smfalse;
        }

        state=Upload;
        uploadIndex=0;
        *progress=5;
    }
    else if(state==Upload)
    {
        size/=2;//bytes to 16 bit words

        //upload data in 32=BL_CHUNK_LEN word chunks
        for(;uploadIndex<size;)
        {
            smAppendSMCommandToQueue( smhandle, SMPCMD_SETPARAMADDR, SMP_BOOTLOADER_UPLOAD );
            for(c=0;c<BL_CHUNK_LEN;c++)
            {
                smuint16 upword;
                //pad end of file with constant to make full chunk
                if(uploadIndex>=size)
                    upword=0xeeee;
                else
                    upword=((smuint16*)data)[uploadIndex];
                smAppendSMCommandToQueue( smhandle, SMPCMD_24B, upword );
                uploadIndex++;
            }

            smAppendSMCommandToQueue( smhandle, SMPCMD_SETPARAMADDR, SMP_BOOTLOADER_FUNCTION );
            smAppendSMCommandToQueue( smhandle, SMPCMD_24B, 2);//BL func 2 = do write on STM32
            smExecuteCommandQueue( smhandle, deviceaddress );

            //read return packets
            for(c=0;c<BL_CHUNK_LEN+3;c++)
            {
                smGetQueuedSMCommandReturnValue( smhandle,&ret );

                if(getCumulativeStatus(smhandle)!=SM_OK)
                {
                    state=Init;
                    return smfalse;
                }
            }

            *progress=5+90*uploadIndex/size;//gives value 5-95
            if(*progress>=94)//95 will indicate that progress is complete. dont let it indicate that yet.
                *progress=94;

            if(uploadIndex%256==0)
            {
                //printf("upload %d\n",uploadIndex);
                return smtrue;//in progress. return often to make upload non-blocking
            }
        }
        if(uploadIndex>=size)//finished
        {
            state=Finish;
        }
    }
    else if(state==Finish)
    {
        //verify STM32 flash if supported by BL version
        if(fwVersion>=1210)
        {
            smSetParameter(smhandle,deviceaddress,SMP_BOOTLOADER_FUNCTION,3);//BL func 3 = verify STM32 FW integrity
            smint32 faults;
            smRead1Parameter(smhandle,deviceaddress,SMP_FAULTS,&faults);

            if(getCumulativeStatus(smhandle)!=SM_OK)
            {
                state=Init;
                *progress=0;
                return smfalse;
            }

            if(faults&FLT_FLASHING_COMMSIDE_FAIL)
            {
                //printf("verify failed\n");
                *progress=0;
                state=Init;
                return smfalse;
            }
            else
            {
                //printf("verify success\n");
            }
        }

        *progress=95;//my job is complete
        state=Init;
    }

    return smtrue;
}


typedef enum { StatIdle=0, StatEnterDFU, StatFindDFUDevice, StatLoadFile, StatUpload, StatLaunch } UploadState;//state machine status

//handle error in FW upload
FirmwareUploadStatus abortFWUpload( FirmwareUploadStatus stat, UploadState *state, int errorDetailCode )
{
    globalErrorDetailCode=errorDetailCode;
    *state=StatIdle;
    return stat;
}

/**
 * @brief smFirmwareUpload Sets drive in firmware upgrade mode if necessary and uploads a new firmware. Call this many until it returns value 100 (complete) or a negative value (error).
 * @param smhandle SM bus handle, must be opened before call
 * @param smaddress Target SM device address. Can be device in DFU mode or main operating mode. For Argon, one device in a bus must be started into DFU mode by DIP switches and smaddress must be set to 255.
 * @param filename .gdf file name
 * @return Enum FirmwareUploadStatus that indicates errors or Complete status. Typecast to integer to get progress value 0-100.
 */
FirmwareUploadStatus smFirmwareUpload( const smbus smhandle, const int smaddress, const char *firmware_filename )
{
    static smuint8 *fwData=NULL;
    static int fwDataLength;
    static smbool fileLoaded=smfalse;
    FirmwareUploadStatus state;

    //load file to buffer if not loaded yet
    if(fileLoaded==smfalse)
    {
        if(loadBinaryFile(firmware_filename,&fwData,&fwDataLength)!=smtrue)
            return FWFileNotReadable;
        fileLoaded=smtrue;
    }

    //update FW, called multiple times per upgrade
    state=smFirmwareUploadFromBuffer( smhandle, smaddress, fwData, fwDataLength );

    //if process complete, due to finish or error -> unload file.
    if(((int)state<0 || state==FWComplete) && fileLoaded==smtrue)
    {
        free(fwData);
        fileLoaded=smfalse;
    }

    return state;
}


/**
 * @brief smFirmwareUpload Sets drive in firmware upgrade mode if necessary and uploads a new firmware. Call this many until it returns value 100 (complete) or a negative value (error).
 * @param smhandle SM bus handle, must be opened before call
 * @param smaddress Target SM device address. Can be device in DFU mode or main operating mode. For Argon, one device in a bus must be started into DFU mode by DIP switches and smaddress must be set to 255.
 * @param fwData pointer to memory address where .gdf file contents are loaded. Note: on some architectures (such as ARM Cortex M) fwData must be aligned to nearest 4 byte boundary to avoid illegal machine instructions.
 * @param fwDataLenght number of bytes in fwData
 * @return Enum FirmwareUploadStatus that indicates errors or Complete status. Typecast to integer to get progress value 0-100.
 */
FirmwareUploadStatus smFirmwareUploadFromBuffer( const smbus smhandle, const int smaddress, smuint8 *fwData, const int fwDataLength )
{
    static smuint32 primaryMCUDataOffset, primaryMCUDataLenth;
    static smuint32 secondaryMCUDataOffset,secondaryMCUDataLength;
    static UploadState state=StatIdle;//state machine status
    static smint32 deviceType=0;
    static int DFUAddress;
    static int progress=0;

    SM_STATUS stat;

    //state machine
    if(state==StatIdle)
    {

        //check if device is in DFU mode already
        smint32 busMode;
        stat=smRead2Parameters(smhandle,smaddress,SMP_BUS_MODE,&busMode, SMP_DEVICE_TYPE, &deviceType);
        if(stat==SM_OK && busMode==SMP_BUS_MODE_DFU)
        {
            state=StatLoadFile;
        }
        else if(stat==SM_OK && busMode!=0)//device not in bus mode
        {
            if(deviceType==4000)//argon does not support restarting in DFU mode by software
            {
                return abortFWUpload(FWConnectionError,&state,200);
            }

            //restart device into DFU mode
            state=StatEnterDFU;

            stat=smSetParameter(smhandle,smaddress,SMP_SYSTEM_CONTROL,64);//reset device to DFU command
            if(stat!=SM_OK)
                return abortFWUpload(FWConnectionError,&state,300);
        }
        else
            state=StatFindDFUDevice;//search DFU device in brute force, fallback for older BL versions that don't preserve same smaddress than non-DFU mode
            //return abortFWUpload(FWConnectionError,fwData,&state,301);

        progress=1;
        DFUAddress=smaddress;
    }

    else if(state==StatEnterDFU)
    {
        sleep_ms(2500);//wait device to reboot in DFU mode. probably shorter delay would do.

        //check if device is in DFU mode already
        smint32 busMode;
        stat=smRead2Parameters(smhandle,smaddress, SMP_BUS_MODE,&busMode, SMP_DEVICE_TYPE, &deviceType);
        if(stat==SM_OK && busMode==0)//busmode 0 is DFU mode
        {
            state=StatLoadFile;
        }
        else
            state=StatFindDFUDevice;//search DFU device in brute force, fallback for older BL versions that don't preserve same smaddress than non-DFU mode

        progress=2;
    }

    else if(state==StatFindDFUDevice)
    {
        int i;
        for(i=245;i<=255;i++)
        {
            smint32 busMode;
            stat=smRead2Parameters(smhandle,i, SMP_BUS_MODE,&busMode, SMP_DEVICE_TYPE, &deviceType);
            if(stat==SM_OK && busMode==0)//busmode 0 is DFU mode
            {
                state=StatLoadFile;
                DFUAddress=i;
                break;//DFU found, break out of for loop
            }
        }

        if(i==256)//DFU device not found
            return abortFWUpload(FWConnectingDFUModeFailed,&state,400);//setting DFU mode failed

        progress=3;
    }

    else if(state==StatLoadFile)
    {
        FirmwareUploadStatus stat=verifyFirmwareData(fwData, fwDataLength, deviceType,
                                  &primaryMCUDataOffset, &primaryMCUDataLenth,
                                  &secondaryMCUDataOffset, &secondaryMCUDataLength);
        if(stat!=FWComplete)//error in verify
        {
            return abortFWUpload(stat,&state,100);
        }

        //all good, upload firmware
        state=StatUpload;

        progress=4;
    }

    else if(state==StatUpload)
    {
        smbool ret=flashFirmwarePrimaryMCU(smhandle,DFUAddress,fwData+primaryMCUDataOffset,primaryMCUDataLenth,&progress);
        if(ret==smfalse)//failed
        {
            return abortFWUpload(FWConnectionError,&state,1000);
        }
        else
        {
            if(progress>=95)
                state=StatLaunch;
        }
    }

    else if(state==StatLaunch)
    {
        smSetParameter(smhandle,DFUAddress,SMP_BOOTLOADER_FUNCTION,4);//BL func 4 = launch.
        sleep_ms(2000);
        progress=100;
        state=StatIdle;
    }

    return (FirmwareUploadStatus)progress;
}





