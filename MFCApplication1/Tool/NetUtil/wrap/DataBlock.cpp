﻿#include "datablock.h"
#include <string.h>

namespace NetworkUtil
{
	int DataBlock::copy(unsigned int pos,const char *buf,unsigned int buflen)
	{
		if(!buf || !buflen)
			return -1;

		unsigned int tmppos = pos +  buflen;
		//未超出容量
		if(  tmppos <= m_size )
		{
			memcpy(m_buf+pos,buf,buflen);
			m_pos = tmppos;
		}
		else 
		{
			unsigned int newSize = m_size;
			while(newSize < tmppos)
				newSize = newSize << 2;
			
			char *tmpbuf = new char[newSize];
			if(!tmpbuf)
				return 0;

			memcpy(tmpbuf,m_buf,m_pos);
			delete []m_buf;
			memcpy(tmpbuf + pos,buf,buflen);

			m_buf = tmpbuf;
			m_pos = tmppos;
			m_size = newSize;
		}

		return buflen;
	}
}