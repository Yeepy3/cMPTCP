#! /usr/bin/python
from selenium import webdriver
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.chrome.options import Options
import time
import datetime
import os
import sys

if(len(sys.argv)<3):
	print ("Usage : "+sys.argv[0]+" <exp_id> <WebPage> <Duration>")
	exit(0)

exp_id = sys.argv[1]
webpage = sys.argv[2]
duration = int(sys.argv[3])
newpath="/home/sandesh/workspace/journal/dynamic/DASH"

#chome-options
options = Options()
#chrome_options.add_argument('--headless')
options.add_argument('--no-sandbox')
#chrome_options.add_argument('--disable-dev-shm-usage')

# Open video
driver = webdriver.Chrome(executable_path="/usr/bin/chromedriver",chrome_options=options)
#driver = webdriver.Chrome(executable_path="/usr/bin/chromedriver",chrome_options=chrome_options)
driver.get("http://192.168.1.112/DASH/dash.js/samples/advanced/monitoring.html")
timer=driver.find_element_by_id("time");
bufferl = driver.find_element_by_id("bufferLevel")
framerate=driver.find_element_by_id("framerate")
reportedBitrate=driver.find_element_by_id("reportedBitrate")
averagethroughput=driver.find_element_by_id("averagethroughput")

try:
	stats_fd = open(newpath+"/"+exp_id+"_stats.txt",'w')

	#play video

	# Record Content
	for i in range(0,duration):
            writer=""+timer.get_attribute('innerText')+" "+bufferl.get_attribute('innerText')+" "+framerate.get_attribute('innerText')+" "+reportedBitrate.get_attribute('innerText')+" "+averagethroughput.get_attribute('innerText')+"\n"
            stats_fd.write(writer)
	    time.sleep(1)

	#Close Files
	stats_fd.close()
	driver.close()
	

except Exception as e:
	driver.close()
	os.remove(newpath+"/"+exp_id+"_stats.txt")
	print(e)




