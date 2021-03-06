
#include "guid_container.h"

#include <vector>

class guid_container_i : public guid_container
{
	struct item
	{
		unsigned ref_count;
		unsigned serial;
		unsigned long vidpid;
	};

	std::vector< item > list;

	unsigned            serial;

public:
	guid_container_i() : serial( 0 ) {}
	virtual ~guid_container_i() {}

	virtual unsigned add( const unsigned long & guid )
	{
		std::vector< item >::iterator it;
		for ( it = list.begin(); it < list.end(); ++it )
		{
			if ( it->vidpid == guid )
			{
				++ it->ref_count;
				return it->serial;
			}
		}

		item i;
		i.ref_count = 1;
		i.serial = serial++;
		i.vidpid = guid;

		list.push_back( i );

		return i.serial;
	}

	virtual void remove( const unsigned long & guid )
	{
		std::vector< item >::iterator it;
		for ( it = list.begin(); it < list.end(); ++it )
		{
			if ( it->vidpid == guid )
			{
				if ( -- it->ref_count == 0 )
				{
					list.erase( it );
				}
				break;
			}
		}
	}

	virtual bool get_guid( unsigned serial, unsigned long & guid )
	{
		std::vector< item >::iterator it;
		for ( it = list.begin(); it < list.end(); ++it )
		{
			if ( it->serial == serial )
			{
				guid = it->vidpid;
				return true;
			}
		}
		return false;
	}
};

guid_container * create_guid_container()
{
	return new guid_container_i;
}