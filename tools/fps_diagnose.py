#!/usr/bin/env python

import re
import argparse

def analyze_log_file(file_path, threshold_ms=16.666):
    """分析日志文件，找出相邻行时间间隔超过阈值的行对

    Args:
        file_path (str): 日志文件路径
        threshold_ms (float): 时间间隔阈值（毫秒）
    """
    try:
        with open(file_path, 'r') as file:
            lines = file.readlines()
    except FileNotFoundError:
        print(f"错误：文件 '{file_path}' 不存在")
        return []
    except Exception as e:
        print(f"读取文件时出错: {e}")
        return []

    timestamp_pattern = re.compile(r'\[\s*(\d+\.\d+)\]')
    results = []

    for i in range(len(lines)-1):
        line1 = lines[i].strip()
        line2 = lines[i+1].strip()

        match1 = timestamp_pattern.search(line1)
        match2 = timestamp_pattern.search(line2)

        if match1 and match2:
            try:
                time1 = float(match1.group(1))
                time2 = float(match2.group(1))
                time_diff = (time2 - time1) * 1000  # 转换为毫秒

                if time_diff > threshold_ms:
                    results.append({
                        'line_num': (i+1, i+2),  # 行号(从1开始)
                        'timestamps': (time1, time2),
                        'time_diff_ms': time_diff,
                        'contents': (line1, line2)
                    })
            except ValueError:
                continue

    return results

def print_results(results, file_path, threshold):
    """打印分析结果"""
    print(f"\n分析文件: {file_path}")
    print(f"时间间隔阈值: {threshold} ms")
    print(f"共找到 {len(results)} 处间隔超过阈值的行对:")
    print("=" * 80)

    for idx, result in enumerate(results, 1):
        line1, line2 = result['line_num']
        time_diff = result['time_diff_ms']
        content1, content2 = result['contents']

        print(f"{idx}. {line1} → {line2} (Delta: {time_diff:.3f} ms)")
        print(f"   {line1} {content1}")
        print(f"   {line2} {content2}")
        print("-" * 80)

def main():
    # 设置命令行参数
    parser = argparse.ArgumentParser(
        description='分析日志文件中相邻行的时间间隔',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument('file', help='要分析的日志文件路径')
    parser.add_argument('-t', '--threshold', type=float, default=16.666,
                       help='时间间隔阈值（毫秒）')
    parser.add_argument('-o', '--output', help='将结果保存到指定文件')

    args = parser.parse_args()

    # 执行分析
    results = analyze_log_file(args.file, args.threshold)

    # 输出结果
    if results:
        print_results(results, args.file, args.threshold)

        # 如果需要保存到文件
        if args.output:
            with open(args.output, 'w') as f:
                # 重定向print输出到文件
                import sys
                original_stdout = sys.stdout
                sys.stdout = f
                print_results(results, args.file, args.threshold)
                sys.stdout = original_stdout
            print(f"\n结果已保存到: {args.output}")
    else:
        print(f"未找到间隔超过 {args.threshold} ms 的行对")

if __name__ == "__main__":
    main()
