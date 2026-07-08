import cv2
import numpy as np
import pprint


# ==========================================
# 检测单个按钮是否有红点
# ==========================================

def detect_red_dot(button_roi):

    h, w = button_roi.shape[:2]


    # --------------------------------------
    # 只检测按钮右上角区域
    # 红点一般位于图标右上方
    # --------------------------------------

    check_roi = button_roi[
        0:int(h * 0.45),
        int(w * 0.45):w
    ]


    hsv = cv2.cvtColor(
        check_roi,
        cv2.COLOR_BGR2HSV
    )


    # 红色HSV范围

    lower_red1 = np.array(
        [0, 80, 80]
    )

    upper_red1 = np.array(
        [10, 255, 255]
    )


    lower_red2 = np.array(
        [170, 80, 80]
    )

    upper_red2 = np.array(
        [180, 255, 255]
    )


    mask1 = cv2.inRange(
        hsv,
        lower_red1,
        upper_red1
    )


    mask2 = cv2.inRange(
        hsv,
        lower_red2,
        upper_red2
    )


    mask = mask1 + mask2



    # 去除噪声

    kernel = np.ones(
        (3,3),
        np.uint8
    )


    mask = cv2.morphologyEx(
        mask,
        cv2.MORPH_OPEN,
        kernel
    )



    # 查找红色区域

    contours, _ = cv2.findContours(
        mask,
        cv2.RETR_EXTERNAL,
        cv2.CHAIN_APPROX_SIMPLE
    )


    for cnt in contours:


        area = cv2.contourArea(cnt)


        # 过滤小噪点

        if area < 30:
            continue



        x,y,w,h = cv2.boundingRect(cnt)


        ratio = w/h if h != 0 else 0



        # 红点接近圆形

        if 0.5 < ratio < 2:


            center_x = x + w//2
            center_y = y + h//2


            return True, [
                center_x,
                center_y
            ]



    return False, None




# ==========================================
# 主函数
# ==========================================

def detect_bottom_buttons(image_path):


    img = cv2.imread(
        image_path
    )


    if img is None:

        return {
            "error":
            "图片读取失败"
        }



    height, width = img.shape[:2]


    print(
        "图片尺寸:",
        width,
        height
    )



    # ======================================
    # 1. 获取底部5个按钮坐标
    # ======================================


    button_names = [
        "首页",
        "市集",
        "发布",
        "消息",
        "我"
    ]


    button_width = width / 5


    buttons = []


    for i,name in enumerate(button_names):


        x1 = int(
            i * button_width
        )

        x2 = int(
            (i+1) * button_width
        )


        center_x = int(
            (x1+x2)/2
        )


        center_y = int(
            height * 0.96
        )


        buttons.append({

            "name": name,

            "x1": x1,

            "x2": x2,

            "center": [
                center_x,
                center_y
            ]

        })



    # ======================================
    # 2. 裁剪底部导航区域
    # ======================================


    bottom_y1 = int(
        height * 0.95
    )


    bottom_y2 = height



    bottom_img = img[
        bottom_y1:bottom_y2,
        :
    ]



    # 保存裁剪结果

    cv2.imwrite(
        "bottom_debug.png",
        bottom_img
    )


    print(
        "已保存底部区域: bottom_debug.png"
    )



    # ======================================
    # 3. 检测每个按钮红点
    # ======================================


    results = []


    for btn in buttons:


        # 当前按钮截图

        roi = bottom_img[
            :,
            btn["x1"]:btn["x2"]
        ]



        has_red, red_local = detect_red_dot(
            roi
        )



        red_global = None



        if has_red:


            red_global = [

                btn["x1"]
                +
                red_local[0],


                bottom_y1
                +
                red_local[1]

            ]



        results.append({

            "button":
            btn["name"],


            "center":
            btn["center"],


            "has_red_dot":
            has_red,


            "red_dot_center":
            red_global

        })



    # ======================================
    # 测试阶段
    # 返回全部按钮
    # ======================================


    return {

        "image_size":
        [
            width,
            height
        ],


        "bottom_area":
        [
            0,
            bottom_y1,
            width,
            bottom_y2
        ],


        "buttons":
        results

    }





# ==========================================
# 测试入口
# ==========================================


if __name__ == "__main__":


    result = detect_bottom_buttons(
        r"D:\xiaoyao\rk\0_13.jpg"
    )


    pprint.pprint(
        result
    )