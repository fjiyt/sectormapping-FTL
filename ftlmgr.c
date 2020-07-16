// 주의사항
// 1. sectormap.h에 정의되어 있는 상수 변수를 우선적으로 사용해야 함
// 2. sectormap.h에 정의되어 있지 않을 경우 본인이 이 파일에서 만들어서 사용하면 됨
// 3. 필요한 data structure가 필요하면 이 파일에서 정의해서 쓰기 바람(sectormap.h에 추가하면 안됨)

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include "sectormap.h"
// 필요한 경우 헤더 파일을 추가하시오.
typedef struct
{
	int psn;
	int lsn;
}MapTblEntry;

typedef struct
{
	MapTblEntry entry[DATAPAGES_PER_DEVICE];
}MapTbl;

typedef struct node
{
	struct node *next;
	int data;
}NODE;
FILE *flashfp;
MapTbl maptbl;
SpareData sparedata[BLOCKS_PER_DEVICE*PAGES_PER_BLOCK];
//빈 block모아둔 연결리스트
NODE* fhead=NULL;
//garbage block모아둔 연결리스트
NODE* ghead=NULL;

int dd_read(int ppn,char *pagebuf);
int dd_write(int ppn,char *pagebuf);
int dd_erase(int pbn);

int free_index=DATABLKS_PER_DEVICE;//free block index

void ftl_open();
void ftl_read(int lsn,char *sectorbuf);
void ftl_write(int lsn,char *sectorbuf);
void init_list(NODE*);
NODE* create_node(int);
void insert_node(NODE*,int);
void remove_node(NODE*);
int find_freepage();
void copy_data(int);
void ftl_print();
//
// flash memory를 처음 사용할 때 필요한 초기화 작업, 예를 들면 address mapping table에 대한
// 초기화 등의 작업을 수행한다. 따라서, 첫 번째 ftl_write() 또는 ftl_read()가 호출되기 전에
// file system에 의해 반드시 먼저 호출이 되어야 한다.
//
void ftl_open()
{
	//
	// address mapping table 초기화
	// free block's pbn 초기화
    // address mapping table에서 lbn 수는 DATABLKS_PER_DEVICE 동일
	int i;
	int num;
	for(i=0;i<DATAPAGES_PER_DEVICE;i++)
	{
		maptbl.entry[i].lsn=i;
		maptbl.entry[i].psn=-1;
	}
	free_index=DATABLKS_PER_DEVICE;
	//SpareData 구조체배열 초기화
	for(i=0;i<DATAPAGES_PER_DEVICE;i++)//freeblock이 아닌 페이지 초기화
	{
		sparedata[i].lpn=0;
		sparedata[i].is_invalid=-1;
	}
	for(i=0;i<PAGES_PER_BLOCK;i++)//freeblock에 해당하는 페이지 초기화
	{
		num=DATAPAGES_PER_DEVICE+i;
		sparedata[num].lpn=-1;
		sparedata[num].is_invalid=-1;
	}
	//empty block list 생성. 처음에는 다 빈페이지이므로 freeblock을 뺀 나머지를 할당
	fhead=(NODE*)malloc(sizeof(NODE));
	fhead->next=NULL;
	NODE* curr=fhead;
	for(i=DATABLKS_PER_DEVICE-1;i>=0;i--)
	{
		insert_node(curr,i);
	}//freeblock은 제외
	//garbage block list 생성.처음에는 garbage block이 없으니깐 초기화만
	ghead=(NODE*)malloc(sizeof(NODE));
	ghead->next=NULL;
	return;
}
//
// 이 함수를 호출하기 전에 이미 sectorbuf가 가리키는 곳에 512B의 메모리가 할당되어 있어야 한다.
// 즉, 이 함수에서 메모리를 할당받으면 안된다.
//
void ftl_read(int lsn, char *sectorbuf)//해당 lsn에 데이터를 sectorbuf로 옮김
{
	int ppn;
	int ret; //리턴값 저장
	char pagebuf[PAGE_SIZE];

	if(lsn>=DATAPAGES_PER_DEVICE)
	{
		fprintf(stderr,"please check your lsn size!\n");
		exit(1);
	}
	ppn=maptbl.entry[lsn].psn;
	if(ppn==-1)
	{
		fprintf(stderr,"No data exist!\n");
		exit(1);
	}
	else
	{
		if((ret=dd_read(ppn,pagebuf))==-1)
		{
			fprintf(stderr,"flash memory read error\n");
			exit(1);
		}
		memset(sectorbuf,0,SECTOR_SIZE);
		memcpy(sectorbuf,pagebuf,SECTOR_SIZE);

	}
	return;
}

void ftl_write(int lsn, char *sectorbuf)
{
	int new_ppn,old_ppn;
	int gbn;
	char pagebuf[PAGE_SIZE];
	char temp_buf[PAGE_SIZE];
	char temp_sectorbuf[PAGE_SIZE];
	char temp_sparebuf[SPARE_SIZE];
	char freebuf[PAGE_SIZE];
	memset(freebuf,0xFF,PAGE_SIZE);

	if(lsn>=DATAPAGES_PER_DEVICE)
	{//lsn 범위 초과시 에러
		fprintf(stderr,"please check your lsn size!\n");
		exit(1);
	}

	if(maptbl.entry[lsn].psn!=-1)//덮어쓰기일 경우
	{
	new_ppn=find_freepage();
		old_ppn=maptbl.entry[lsn].psn;//해당 lsn에 대한 원래 ppn
		sparedata[old_ppn].is_invalid=1;//이 lsn에 대해 덮어쓰기가 되었다는 의미
		sparedata[old_ppn].lpn=lsn;
		//garbage block list에 추가
		gbn=old_ppn/PAGES_PER_BLOCK;
		NODE* gcurr=ghead;
		insert_node(gcurr,gbn);
		if(new_ppn==-1)
		{
			new_ppn=find_freepage();
		}
	}
	else
		new_ppn=find_freepage();
	//newppn에 정보입력
	memset(pagebuf,0xFF,PAGE_SIZE);
	memset(temp_sectorbuf,0xFF,SECTOR_SIZE);
	memcpy(temp_sectorbuf,sectorbuf,strlen(sectorbuf));//입력받은 섹터데이터값 받기
	memcpy(pagebuf,temp_sectorbuf,SECTOR_SIZE);//페이지에 섹터데이터 추가
	pagebuf[SECTOR_SIZE]=lsn;
	
	sparedata[new_ppn].lpn=lsn;//스페어값 설정
	sparedata[new_ppn].is_invalid=0;//해당 ppn에 데이터가 입력됨을 의미

	maptbl.entry[lsn].psn=new_ppn;
	dd_write(new_ppn,pagebuf);

	return;
}
void insert_node(NODE* target,int data)
{
	NODE* newNode=(NODE*)malloc(sizeof(NODE));
	newNode->data=data;
	newNode->next=target->next;
	target->next=newNode;
}
void remove_node(NODE* target)
{
	NODE* removeNode=target->next;
	target->next=removeNode->next;

	free(removeNode);
}
int find_freepage()
{
	int i;
	int fpbn=0,gpbn=0;
	int ppn=-1;
	int fppn=0,gppn=0;
	int gbn;//garbage block number
	char temp_buf[PAGE_SIZE];
	char copy_buf[PAGE_SIZE];
	
	NODE* fcurr=fhead->next;//empty block list 노드 이동
	while(fcurr!=NULL)
	{
		fpbn=fcurr->data;//pbn
		fppn=fpbn*PAGES_PER_BLOCK;//해당하는 ppn
		for(i=0;i<PAGES_PER_BLOCK;i++)
		{
			if(i==PAGES_PER_BLOCK-1)
			{
				NODE* head=fhead;
				remove_node(head);
				fcurr=fhead->next;
			}
			if(sparedata[fppn+i].lpn>=0&&sparedata[fppn+i].is_invalid==-1){
				ppn=fppn+i;
				break;
			}
		}
		if(ppn!=-1)
			break;
	}	

	if(ppn==-1)//더이상 freepage가 없는경우. garbage block을 찾아야됨
	{
		NODE* gcurr=ghead->next;
		while(gcurr!=NULL)
		{
			gpbn=gcurr->data;//gpbn
			gppn=gpbn*PAGES_PER_BLOCK;//해당하는 ppn
			for(i=0;i<PAGES_PER_BLOCK;i++)//필요없는 데이터찾기(덮어씌워진 데이터)
			{
				if(sparedata[gppn+i].is_invalid==1){
					ppn=free_index*PAGES_PER_BLOCK+i;
					break;
				}
			}
			NODE* head2=ghead;
			remove_node(head2);
			gcurr=ghead->next;
			if(ppn!=-1)
				break;
		}
		if(ppn!=-1)
		{
			copy_data(gpbn);//필요있는 데이터 옮기기
			//new freeblock을 empty block list로 옮김
			fcurr=fhead;
			insert_node(fcurr,free_index);
			free_index=gpbn;//garbage block이 new freeblock이 됨
			dd_erase(gpbn);
		}
	}
		return ppn;
}
void copy_data(int gpbn)
{
	int gppn=gpbn*PAGES_PER_BLOCK;
	int fppn=free_index*PAGES_PER_BLOCK;
	char copy_buf[PAGE_SIZE];
	int i,lsn;

	for(i=0;i<PAGES_PER_BLOCK;i++)
	{
		if(sparedata[gppn+i].is_invalid==0)//삭제되면 안되는 데이터
		{//freeblock으로 옮겨야됨
			dd_read(gppn+i,copy_buf);
			dd_write(fppn+i,copy_buf);
			lsn=sparedata[gppn+i].lpn;
			sparedata[fppn+i].is_invalid=0;
			sparedata[fppn+i].lpn=lsn;//이동
			sparedata[gppn+i].is_invalid=-1;//빈공간
			sparedata[gppn+i].lpn=-1;//freeblock page를 의미함
			maptbl.entry[lsn].psn=fppn+i;
		}
		else
		{
			sparedata[fppn+i].is_invalid=-1;
			sparedata[fppn+i].lpn=0;//이동
			sparedata[gppn+i].is_invalid=-1;//빈공간
			sparedata[gppn+i].lpn=-1;//freeblock page를 의미함
		}

	}
}
void ftl_print()//mapping table 출력에 필요한 함수
{
	printf("lpn ppn\n");
	for(int i=0;i<DATAPAGES_PER_DEVICE;i++)
	{
		printf("%d %d\n",maptbl.entry[i].lsn,maptbl.entry[i].psn);
	}
	printf("free block's pbn=%d\n",free_index);
	return;
}
