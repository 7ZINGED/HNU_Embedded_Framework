 /*
 * Change Logs:
 * Date            Author          Notes
 * 2023-08-23      ChuShicheng     first version
 */
#include "dji_motor.h"
#include "rm_config.h"
#include "usr_callback.h"
#include "rm_task.h"
#define DBG_TAG   "dji.motor"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>
float power__all=0;
#define DJI_MOTOR_CNT 14             // 默认波特率下，实测挂载电机极限数量
/* 滤波系数设置为1的时候即关闭滤波 */
#define SPEED_SMOOTH_COEF 0.85f      // 最好大于0.85
#define CURRENT_SMOOTH_COEF 0.9f     // 必须大于0.9
#define ECD_ANGLE_COEF_DJI 0.043945f // (360/8192),将编码器值转化为角度制
#define  constant 0.675f//2650
#define k_rpm 1.453e-07//0.001651
#define k_current 1.23e-07//0.000000001721
#define k_Torque 1.997e-06f
static uint8_t idx = 0; // register idx,是该文件的全局电机索引,在注册时使用
/* DJI电机的实例,此处仅保存指针,内存的分配将通过电机实例初始化时通过malloc()进行 */
static dji_motor_object_t *dji_motor_obj[DJI_MOTOR_CNT] = {NULL};
 static struct referee_fdb_msg referee_fdb;
static rt_device_t chassis_can, gimbal_can;

// TODO: 0x2ff容易发送失败
/**
 *
 * @brief 由于DJI电机发送以四个一组的形式进行,故对其进行特殊处理,用6个(2can*3group)can_object专门负责发送
 *        该变量将在 dji_motor_control() 中使用,分组在 motor_send_grouping()中进行
 *
 * C610(m2006)/C620(m3508):0x1ff,0x200;
 * GM6020:0x1ff,0x2ff
 * 反馈(rx_id): GM6020: 0x204+id ; C610/C620: 0x200+id
 * can1: [0]:0x1FF,[1]:0x200,[2]:0x2FF
 * can2: [3]:0x1FF,[4]:0x200,[5]:0x2FF
 */
static struct rt_can_msg send_msg[6] = {
    [0] = {.id = 0x1ff, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
    [1] = {.id = 0x200, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
    [2] = {.id = 0x2ff, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
    [3] = {.id = 0x1ff, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
    [4] = {.id = 0x200, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
    [5] = {.id = 0x2ff, .ide  = RT_CAN_STDID, .rtr = RT_CAN_DTR, .len  = 0x08, .data = {0}},
};

/**
 * @brief 6个用于确认是否有电机注册到sender_assignment中的标志位,防止发送空帧,此变量将在dji_motor_control()使用
 *        flag的初始化在 motor_send_grouping()中进行,012为底盘can,345为云台can
 */
static uint8_t sender_enable_flag[6] = {0};

/**
 * @brief 根据电调/拨码开关上的ID,根据说明书的默认id分配方式计算发送ID和接收ID,
 *        并对电机进行分组以便处理多电机控制命令
 */
 static void motor_send_grouping(dji_motor_object_t *motor, motor_config_t *config)
 {
     uint8_t motor_id = 0;    // 电机实际ID，如1,2,3,4
     uint8_t motor_send_num = 0;
     uint8_t motor_group = 0;
     /* 用于判断can设备名称是否相同 */
     static const char *can_chassis = CAN_CHASSIS;
     static const char *can_gimbal = CAN_GIMBAL;

     switch (motor->motor_type)
     {
     case M2006:
     case M3508:
         motor_id = motor->rx_id - 0x200; // 下标从零开始,先减一方便赋值
         if (motor_id <= 4) // 根据ID分组
         {
             motor_send_num = motor_id - 1;
             motor_group = rt_strcmp(config->can_name, can_chassis) ? 4 : 1;
         }
         else
         {
             motor_send_num = motor_id - 5;
             motor_group = rt_strcmp(config->can_name, can_chassis) ? 3 : 0;
         }

         // 计算接收id并设置分组发送id
         sender_enable_flag[motor_group] = 1; // 设置发送标志位,防止发送空帧
         motor->message_num = motor_send_num;
         motor->send_group = motor_group;

         // 检查是否发生id冲突
         for (size_t i = 0; i < idx; ++i)
         {
             if (!rt_strcmp(dji_motor_obj[i]->can_dev->parent.name, config->can_name) && dji_motor_obj[i]->rx_id == config->rx_id)
             {
                 LOG_E("ID crash. Check in debug mode, add dji_motor_obj to watch to get more information.");
                 while (1) // 6020的id 1-4和2006/3508的id 5-8会发生冲突(若有注册,即1!5,2!6,3!7,4!8)
                     LOG_E("id [%d], can_bus [%s]", config->rx_id, config->can_name);
             }
         }
         break;

     case GM6020:
         motor_id = motor->rx_id - 0x204; // 下标从零开始,先减一方便赋值
         if (motor_id <= 4)
         {
             motor_send_num = motor_id - 1;
             motor_group = rt_strcmp(config->can_name, can_chassis) ? 3 : 0;
         }
         else
         {
             motor_send_num = motor_id - 5;
             motor_group = rt_strcmp(config->can_name, can_chassis) ? 5 : 2;
         }

         sender_enable_flag[motor_group] = 1; // 只要有电机注册到这个分组,置为1;在发送函数中会通过此标志判断是否有电机注册
         motor->message_num = motor_send_num;
         motor->send_group = motor_group;

         for (size_t i = 0; i < idx; ++i)
         {
             if (!rt_strcmp(dji_motor_obj[i]->can_dev->parent.name, config->can_name) && dji_motor_obj[i]->rx_id == config->rx_id)
             {
                 LOG_E("ID crash. Check in debug mode, add dji_motor_obj to watch to get more information.");
                 while (1) // 6020的id 1-4和2006/3508的id 5-8会发生冲突(若有注册,即1!5,2!6,3!7,4!8)
                     LOG_E("id [%d], can_bus [%s]", config->rx_id, config->can_name);
             }
         }
         break;

     default: // other motors should not be registered here
         while (1)
             LOG_E("You must not register other motors using the API of DJI motor."); // 其他电机不应该在这里注册
     }
 }

/**
 * @brief 根据返回的can_object对反馈报文进行解析
 *
 * @param object 收到数据的object,通过遍历与所有电机进行对比以选择正确的实例
 */
static void decode_dji_motor(dji_motor_object_t *motor, uint8_t *data)
{
    uint8_t *rxbuff = data;
    dji_motor_measure_t *measure = &motor->measure; // measure要多次使用,保存指针减小访存开销

    rt_timer_start(motor->timer);  // 判定电机未离线，重置电机定时器

    // 解析数据并对电流和速度进行滤波,电机的反馈报文具体格式见电机说明手册
    measure->last_ecd = measure->ecd;
    measure->ecd = ((uint16_t)rxbuff[0]) << 8 | rxbuff[1];
    measure->angle_single_round = ECD_ANGLE_COEF_DJI * (float)measure->ecd;
    measure->speed_rpm = (1.0f - SPEED_SMOOTH_COEF) * measure->speed_rpm + SPEED_SMOOTH_COEF * (float)((int16_t)(rxbuff[2] << 8 | rxbuff[3]));
    measure->speed_aps = (1.0f - SPEED_SMOOTH_COEF) * measure->speed_aps +
                         RPM_2_ANGLE_PER_SEC * SPEED_SMOOTH_COEF * (float)((int16_t)(rxbuff[2] << 8 | rxbuff[3]));
    measure->real_current = (1.0f - CURRENT_SMOOTH_COEF) * measure->real_current +
                            CURRENT_SMOOTH_COEF * (float)((int16_t)(rxbuff[4] << 8 | rxbuff[5]));
    measure->temperature = rxbuff[6];

    // 多圈角度计算,前提是假设两次采样间电机转过的角度小于180°,自己画个图就清楚计算过程了
    if (measure->ecd - measure->last_ecd > 4096)
        measure->total_round--;
    else if (measure->ecd - measure->last_ecd < -4096)
        measure->total_round++;
    measure->total_angle = measure->total_round * 360 + measure->angle_single_round;
}

/**
 * @brief 电机定时器超时回调函数
 * @param motor_ptr
 */
 static void motor_lost_callback(void *motor_ptr)
 {
     dji_motor_object_t *motor = (dji_motor_object_t *)motor_ptr;
//     dji_motor_relax(motor);
     LOG_W("[dji_motor] Motor lost, can bus [%s] , id 0x[%x]", motor->can_dev->parent.name, motor->rx_id);
 }

/**
 * @brief 电机反馈报文接收回调函数,该函数被can_rx_call调用
 *
 * @param dev 接收到报文的CAN设备
 * @param id 接收到的报文的id
 * @param data 接收到的报文的数据
 */
void dji_motot_rx_callback(rt_device_t dev, uint32_t id, uint8_t *data){
    // 找到对应的实例后再调用decode_dji_motor进行解析
    for (size_t i = 0; i < idx; ++i)
    {
        if (dji_motor_obj[i]->can_dev == dev && dji_motor_obj[i]->rx_id == id)
        {
            decode_dji_motor(dji_motor_obj[i], data);
        }
    }
}

void dji_motor_relax(dji_motor_object_t *motor)
{
    motor->stop_flag = MOTOR_STOP;
}

void dji_motor_enable(dji_motor_object_t *motor)
{
    motor->stop_flag = MOTOR_ENALBED;
}

// 运算所有电机实例的控制器,发送控制报文
void dji_motor_control()
{
#ifdef BSP_USING_DEFAULT_MODE
     dji_motor_object_t *motor;
     dji_motor_measure_t measure;
     uint8_t group, num; // 电机组号和组内编号
     int16_t set = 0; // 电机控制器计算得到的输出值
     uint8_t size = 0;

     // 遍历所有电机实例,运行控制算法并填入报文
     for (size_t i = 0; i < idx; ++i)
     {
         motor = dji_motor_obj[i];
         measure = motor->measure;

         set = motor->control(measure); // 调用对接的电机控制器计算

         // 分组填入发送数据
         group = motor->send_group;
         num = motor->message_num;
         send_msg[group].data[2 * num] = (uint8_t)(set >> 8);
         send_msg[group].data[2 * num + 1] = (uint8_t)(set & 0x00ff);

         // 若该电机处于停止状态,直接将buff置零
         if (motor->stop_flag == MOTOR_STOP)
             rt_memset(send_msg[group].data + 2 * num, 0, 2 * sizeof(rt_uint8_t));
     }

     // 遍历flag,检查是否要发送这一帧报文
     for (size_t i = 0; i < 6; ++i)
     {
         if (sender_enable_flag[i])
         {
             if(i < 3){
                 size = rt_device_write(chassis_can, 0, &send_msg[i], sizeof(send_msg[i]));
             }
             else{
                 size = rt_device_write(gimbal_can, 0, &send_msg[i], sizeof(send_msg[i]));
             }
             if (size == 0)
             {
                 LOG_W("can dev write data failed!");
             }
         }
     }
#endif
#ifdef BSP_USING_POWER_LIMIT
         dji_motor_object_t *motor;
         dji_motor_measure_t measure;
         uint8_t group, num; // 电机组号和组内编号
         int16_t set = 0; // 电机控制器计算得到的输出值
         uint8_t size = 0;
         int16_t set1[4],rpm[4];
         float power[4]={0},k_zoom = 0,power_all = 0;
         uint8_t group1[4]={0},num1[4]={0};
          // 遍历所有电机实例,运行控制算法并填入报文
     for (size_t i = 0; i < idx; ++i)
         {
         motor = dji_motor_obj[i];
         measure = motor->measure;
         //当电机为底盘电机，即挂载在can1总线且为3508电机，使用底盘功率限制，其余正常填入报文
             if ((motor->motor_type==M3508)&&(motor->can_dev->parent.name[3]==49))
             {
            switch (motor->rx_id) {
                case 0x201:
                    {
                    set1[0] = motor->control(measure);
                    rpm[0] = measure.speed_rpm;
                    group1[0] = motor->send_group;
                    num1[0] = motor->message_num;
                    power[0] = k_rpm * motor->measure.speed_rpm * motor->measure.speed_rpm +
                            k_current * set1[0] * set1[0] +
                            motor->measure.speed_rpm * set1[0] * k_Torque + constant;
                    break;
                    }
                case 0x202:
                    {
                    set1[1] = motor->control(measure);;
                    rpm[1] = measure.speed_rpm;
                    group1[1] = motor->send_group;
                    num1[1] = motor->message_num;
                    power[1] = k_rpm * motor->measure.speed_rpm * motor->measure.speed_rpm +
                               k_current * set1[1] * set1[1] +
                               motor->measure.speed_rpm * set1[1] * k_Torque + constant;
                    break;
                    }
                case 0x203:
                    {
                    set1[2] = motor->control(measure);;
                    rpm[2] = measure.speed_rpm;
                    group1[2] = motor->send_group;
                    num1[2] = motor->message_num;
                    power[2] = k_rpm * motor->measure.speed_rpm * motor->measure.speed_rpm +
                               k_current * set1[2] * set1[2] +
                               motor->measure.speed_rpm * set1[2] * k_Torque + constant;
                    break;
                    }
                case 0x204:
                {
                    rpm[3] = measure.speed_rpm;
                    set1[3] = motor->control(measure);;
                    group1[3] = motor->send_group;
                    num1[3] = motor->message_num;
                    power[3] = k_rpm * motor->measure.speed_rpm * motor->measure.speed_rpm +
                               k_current * set1[3] * set1[3] +
                               motor->measure.speed_rpm * set1[3] * k_Torque + constant;
                    // 计算全局功率限制系数
                    for (int j = 0; j < 4; ++j)
                    {

                        if(power[j]>0) {
                            power_all += power[j];
                        }
                    }
                    power__all=power_all; //用于观察与裁判系统读取功率的拟合效果
                    int powerlimit = 60;
                    int power_limit = referee_fdb.robot_status.chassis_power_limit;
                    if (power_limit >=50 && power_limit <=120){
                        powerlimit = 60;
                    }
                    //底盘功率限制单位转换
                    if (power_all>powerlimit) {
                        k_zoom = powerlimit / power_all;
                        for (int j = 0; j < 4; ++j)
                        {
                            power[j] = k_zoom * power[j] - k_rpm * rpm[j] * rpm[j] - constant;
                            if (set1[j] >= 0) {
                                set1[j] = 0.6*(-k_Torque * rpm[j] +sqrt(k_Torque * rpm[j] * k_Torque * rpm[j] + 4 * power[j] * k_current)) /(2 * k_current);
                                if (set1[j]>6000){
                                    set1[j]=6000;
                                }
                            } else {
                                set1[j] = 0.6*(-k_Torque * rpm[j] -sqrt(k_Torque * rpm[j] * k_Torque * rpm[j] + 4 * power[j] * k_current)) /(2 * k_current);
                                if (set1[j]<-6000){
                                    set1[j]=-6000;
                                }
                            }
                        }
                        power_all = 0;
                    } else
                    {
                        power_all = 0;
                    }
                    for (int j = 0; j < 4; ++j)
                    {
                        send_msg[group1[j]].data[2 * num1[j]] = (uint8_t) (set1[j] >> 8);
                        send_msg[group1[j]].data[2 * num1[j] + 1] = (uint8_t) (set1[j] & 0x00ff);
                    }
                    if (motor->stop_flag == MOTOR_STOP)
                    {
                        for (int j = 0; j < 4; ++j)
                    {
                            rt_memset(send_msg[group1[j]].data + 2 * num1[j], 0, 2 * sizeof(rt_uint8_t));
                    }
                }
             }
             }
             }else{
                 set = motor->control(measure); // 调用对接的电机控制器计算
             // 分组填入发送数据
             group = motor->send_group;
             num = motor->message_num;
             send_msg[group].data[2 * num] = (uint8_t) (set >> 8);
             send_msg[group].data[2 * num + 1] = (uint8_t) (set & 0x00ff);

             // 若该电机处于停止状态,直接将buff置零
             if (motor->stop_flag == MOTOR_STOP)
                 rt_memset(send_msg[group].data + 2 * num, 0, 2 * sizeof(rt_uint8_t));
         }
     }
       // 遍历flag,检查是否要发送这一帧报文
    for (size_t i = 0; i < 6; ++i)
    {
        if (sender_enable_flag[i])
        {
            if(i < 3){
                size = rt_device_write(chassis_can, 0, &send_msg[i], sizeof(send_msg[i]));
            }
            else{
                size = rt_device_write(gimbal_can, 0, &send_msg[i], sizeof(send_msg[i]));
            }
            if (size == 0)
            {
                LOG_W("can dev write data failed!");
            }
        }
    }
#endif
}

/**
 * @brief 电机初始化,返回一个电机实例
 * @param config 电机配置
 * @return dji_motor_object_t* 电机实例指针
 */
dji_motor_object_t *dji_motor_register(motor_config_t *config, void *control)
{
    dji_motor_object_t *object = (dji_motor_object_t *)rt_malloc(sizeof(dji_motor_object_t));
    rt_memset(object, 0, sizeof(dji_motor_object_t));

    // 对接用户配置的 motor_config
    object->motor_type = config->motor_type;                         // 6020 or 2006 or 3508
    object->rx_id = config->rx_id;                                   // 电机接收报文的ID
    object->control = control;                                       // 电机控制器执行

    chassis_can = rt_device_find(CAN_CHASSIS);
    gimbal_can = rt_device_find(CAN_GIMBAL);
    /* 查找 CAN 设备 */
    object->can_dev = rt_device_find(config->can_name);

    // 电机分组,因为至多4个电机可以共用一帧CAN控制报文
    motor_send_grouping(object, config);

    // 电机离线检测定时器相关
    object->timer = rt_timer_create("dji_motor",
                             motor_lost_callback,
                             object, 20,
                             RT_TIMER_FLAG_PERIODIC);
    rt_timer_start(object->timer);

    dji_motor_enable(object);
    dji_motor_obj[idx++] = object;
    return object;
}
