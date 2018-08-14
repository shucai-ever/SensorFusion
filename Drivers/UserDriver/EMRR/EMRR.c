/**
 * microwave radar - EMRR by Electronic Radar(WuHu) 
 * 
 * EMRR_***() functions are entries of this driver
 * ***_func() functions are practical functions called by EMRR_***() functions
 * 
 * ---init & config
 * EMRR_Init(hcan): start CAN2 for radar
 * 
 * //called by EMRR_***() functions mentioned above, no entry outside this file
 * GetRadarObjGeneral_func(...): read Radar Obj General(distance, relVelocity...) from can2
 * 
 * ---read Radar data
 * EMRR_GetRadarObjGeneral(...): get closet Radar Obj General
 *
 * ---calc gyro data
 * EMRR_CalcGyro(...): using gyro data to optimize collision warning
 */
#include "EMRR.h"

#define VEHICLE_CENTRE_LEN	3.5f
#define VEHICLE_HALF_WIDTH	0.9f
#define OBSTACLE_ERR				1.0f

extern uint8_t EMRR_RadarRxComplete;
extern CAN_RxHeaderTypeDef RadarCANRxHeader;

uint8_t EMRR_Init(CAN_HandleTypeDef *hcan)
{
	//配置CAN滤波器接收Objct_General信息，即相对目标的距离、速度等
	//ID_HIGH, ID_LOW,\
	MASK_HIGH, MASK_LOW,\
	FIFO 0/1, filter_bank(0-13/14-27), filter_mode(LIST/MASK), filter_scale, EN/DISABLE filter, SlaveStartFilterBank
	CAN_FilterTypeDef MW_RadarCANFilter = {
		EMRR_OBJ_ADDR<<5, 0,\
		0xFFC0<<5, 0,\
		CAN_FILTER_FIFO0, 14, CAN_FILTERMODE_IDMASK,CAN_FILTERSCALE_32BIT,ENABLE,14};
	HAL_CAN_ConfigFilter(hcan, &MW_RadarCANFilter);
	HAL_CAN_Start(hcan);
	HAL_CAN_ActivateNotification(hcan, CAN_IT_RX_FIFO0_MSG_PENDING);

	return 0;
}

void GetRadarObjGeneral_func(uint8_t* pCANRxBuf, EMRR_RadarGeneral *pRadarGeneral)
{
	uint16_t tempData;
	//id
	pRadarGeneral->trackId = RadarCANRxHeader.StdId- 0x4ff;
	//range
	pRadarGeneral->trackRange = (float)(*pCANRxBuf | ((*(pCANRxBuf + 1) & 0x7F) << 8))*0.01f;
	//speed
	tempData =  ((*(pCANRxBuf + 3) & 0x3F) << 8) | *(pCANRxBuf + 2);
	pRadarGeneral->trackSpeed =  (float)(tempData > 8191 ? (tempData - 16384)*0.01f : tempData*0.01f);
	//angle
	tempData = ((*(pCANRxBuf + 5) & 0xFC) >> 2) | ((*(pCANRxBuf + 6) & 0x1F) << 6);
	pRadarGeneral->trackAngle = (float)((tempData) > 1023 ? (tempData- 2048) *0.1f : tempData *0.1f);
	//power
	if (pRadarGeneral->trackRange > 0)
	{
		tempData = ((*(pCANRxBuf + 6) & 0xE0) >> 5) | ((*(pCANRxBuf + 7) & 0x7F) << 3);
		pRadarGeneral->trackPower = (float)(tempData > 511 ? (tempData - 1024)*0.1f - 40 : (tempData)*0.1f - 40);
	}
	else
	{
		pRadarGeneral->trackPower = 0;
	}
}

void EMRR_GetRaderObjCloset(uint8_t *pCANRxBuf, EMRR_RadarGeneral *pRadarGeneral, EMRR_RadarGeneral *pRadarGeneral_Closet)
{
	if(EMRR_RadarRxComplete)	//接收标志
	{
		GetRadarObjGeneral_func(pCANRxBuf, (pRadarGeneral + RadarCANRxHeader.StdId - 0x500));
		if((RadarCANRxHeader.StdId - 0x500+1) == 64)	//收完64个目标数据
		{
			uint32_t i=0, min_lable;
			float min=1000, rad;
			for(i=0; i<64; i++)
			{
				if((pRadarGeneral + i)->trackRange!=0)	//目标数据不为零
				{
					rad = 3.14 * fabs((pRadarGeneral + i)->trackAngle) / 180;	//角度换算成弧度
					(pRadarGeneral + i)->trackCrossRange = (float)((pRadarGeneral + i)->trackRange * sin(rad));
					if((pRadarGeneral + i)->trackCrossRange < 1.5f)	//左右距离＜1.5米，在车道线内
					{
						if((pRadarGeneral + i)->trackPower> - 35)	//功率
						{
							if(min > (pRadarGeneral + i)->trackRange)	//当前目标距离小于min
							{
								min = (pRadarGeneral + i)->trackRange;
								min_lable = i;
							}
						}
					}
				}
			}
			pRadarGeneral_Closet->trackId = (pRadarGeneral + min_lable)->trackId;
			pRadarGeneral_Closet->trackCrossRange = (pRadarGeneral + min_lable)->trackCrossRange;
			pRadarGeneral_Closet->trackRange = (pRadarGeneral + min_lable)->trackRange;
			pRadarGeneral_Closet->trackSpeed = (pRadarGeneral + min_lable)->trackSpeed;
			pRadarGeneral_Closet->trackAngle = (pRadarGeneral + min_lable)->trackAngle;
			pRadarGeneral_Closet->trackPower = (pRadarGeneral + min_lable)->trackPower;
		}
		EMRR_RadarRxComplete = 0;
	}
}

uint8_t EMRR_CalcTurn(EMRR_RadarGeneral *pRadargGeneral_Closet, float YawRate, float VehicleSpeed)
{
	float Rotate_R, Min_R, Max_R, Obstacle_X, Obstacle_Y, Obstacle_Dis;
	Rotate_R = (VehicleSpeed * 180) / (YawRate * 3.14f);	//Rotate_R = V / ω
	Min_R = Rotate_R - VEHICLE_HALF_WIDTH;
	Max_R = sqrt((Rotate_R + VEHICLE_HALF_WIDTH) * (Rotate_R + VEHICLE_HALF_WIDTH) + \
								 VEHICLE_CENTRE_LEN * VEHICLE_CENTRE_LEN);
	//如果最近位置在最大+一个距离，最小-一个距离之间，就返回1，否则返回0
	Obstacle_X = pRadargGeneral_Closet->trackRange + VEHICLE_CENTRE_LEN;
	Obstacle_Y = -pRadargGeneral_Closet->trackCrossRange + Rotate_R;
	Obstacle_Dis = sqrt(Obstacle_X * Obstacle_X + Obstacle_Y * Obstacle_Y);
	
	if((Obstacle_Dis < Max_R + OBSTACLE_ERR) && (Obstacle_Dis > Min_R - OBSTACLE_ERR))
		return 1;
	else
		return 0;
}

