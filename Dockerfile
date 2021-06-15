FROM ubuntu:20.04

ENV DEBIAN_FRONTEND="noninteractive" 
ENV TZ=Pacific/Auckland

ADD . /kitchen_sync
WORKDIR /kitchen_sync
RUN apt-get update && apt-get -y install build-essential \
		    	cmake \
			libpq-dev \
			libmysqlclient-dev && \
			rm -f /etc/apt/apt.conf.d/20auto-upgrades && \
			apt-get clean -y && \
			rm -rf /var/cache/apt/archives/*

RUN cmake . && make && make install

ENTRYPOINT ["ks"]
